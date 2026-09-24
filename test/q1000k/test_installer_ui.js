// SPDX-License-Identifier: GPL-2.0+
// Unit-test UI routing and destructive-install confirmation with inert DOM/XHR stubs.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../lib/lwip/httpd/htdocs/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const embedded = fs.readFileSync(path.join(__dirname, '../../lib/lwip/httpd/fsdata_xr1710g.c'), 'utf8');
const array = embedded.match(/data__index_html\[\][^=]*= \{([\s\S]*?)\n\};/)[1];
const bytes = Buffer.from(array.split('\n').filter(line=>line.startsWith('0x')).join('').match(/0x[0-9a-f]{2}/g).map(value=>parseInt(value,16)));
const bodyAt = bytes.indexOf('\r\n\r\n')+4;
assert(bytes.subarray(bodyAt).equals(Buffer.from(html)), 'embedded page must match index.html');
assert.match(bytes.subarray(12,bodyAt).toString(),new RegExp('Content-Length: '+Buffer.byteLength(html)+'\\r\\n'));
assert(html.includes('[hidden]{display:none!important}'));
async function scenario(board, ramboot=true) {
  const nodes = new Map();
  function node(id) {
    if (!nodes.has(id)) nodes.set(id, {
      textContent:'', className:'', hidden:true, disabled:false, value:'', style:{},
      classList:{toggle(){},add(){},remove(){}},
      querySelector(){return node('option')},setAttribute(){},removeAttribute(){},
      getAttribute(){return id},addEventListener(){},appendChild(){},removeChild(){},
    });
    return nodes.get(id);
  }
  const requests = [];
  const xhrs = [];
  const prompts = [];
  let confirmations=0, accept=false;
  const context = vm.createContext({
    document:{getElementById:node,querySelectorAll(){return []},createElement:node,body:node('body')},
    location:{protocol:'http:'},
    window:{confirm(message){confirmations++;prompts.push(message);return accept}},
    fetch:async()=>({json:async()=>({board,chainloader_update:true,firmware_mode:'ubi',ramboot,
      ramboot_addr:'0x89000000',ramboot_min_addr:'0x88200000',ramboot_end:'0x9ef00000'})}),
    setInterval(){return 1},clearInterval(){},setTimeout(){},
    XMLHttpRequest:function(){this.upload={addEventListener(){}};this.events={};this.status=200;
      this.addEventListener=(name,fn)=>{this.events[name]=fn};xhrs.push(this);
      this.open=(method,url)=>requests.push({method,url});this.setRequestHeader=()=>{};this.send=()=>{}},
  });
  vm.runInContext(script,context);
  await new Promise(setImmediate);
  if(board==='Q1000K') {
    assert.equal(context.q1000kInstaller,true);
    assert.equal(node('recovery-nav-button').hidden,false);
    context.setTarget('uboot');
    assert.equal(context.ubootMode,'update');
    assert.equal(node('uboot-mode-block').hidden,false);
    assert.equal(node('upload-button').textContent,'Update U-Boot Only');
    assert.match(node('preserve-value').textContent,/All UBI volumes.*vendor environment/);
    assert.equal(node('self-write-button').hidden,true);
    context.selectedFile={size:1000000,name:'chainloader.itb',arrayBuffer:async()=>new ArrayBuffer(0)};
    await context.upload();
    assert.equal(confirmations,1);assert.equal(requests.length,0);assert.equal(context.busy,false);
    accept=true;await context.upload();
    assert.equal(confirmations,2);assert.equal(requests[0].url,'/upload/uboot-only');
    assert.match(prompts[1],/will be preserved/);
    context.setUbootMode('install');assert.equal(context.ubootMode,'update');
    context.setBusy(false);context.setUbootMode('install');
    assert.equal(node('upload-button').textContent,'Install U-Boot and Prepare UBI');
    assert.match(node('scope-value').textContent,/format UBI/);
    accept=false;await context.upload();assert.equal(requests.length,1);
    accept=true;await context.upload();assert.equal(requests[1].url,'/upload/uboot');
    assert.match(prompts[3],/erase all firmware and settings/);
    context.setBusy(false);context.setTarget('recovery');
    assert.equal(node('uboot-mode-block').hidden,true);
    assert.match(node('scope-value').textContent,/erase OpenWrt settings/);
    context.selectedFile.size=2000000;
    await context.upload();assert.equal(requests[2].url,'/upload/recovery');
    context.setBusy(false);context.setTarget('firmware');
    assert.match(node('preserve-value').textContent,/64 KiB factory and recovery/);
    await context.upload();assert.equal(requests[3].url,'/upload/firmware');
    context.setBusy(false);context.setTarget('initramfs');
    assert.equal(node('ramboot-nav-button').hidden,!ramboot);
    if(ramboot){
      assert.equal(context.activeTarget,'initramfs');
      assert.equal(node('upload-button').textContent,'Upload and Boot');
      assert.equal(node('row-erase').hidden,true);
      assert.equal(node('row-write').hidden,true);
      assert.equal(node('layout-block').hidden,true);
      assert.equal(node('ramboot-address-block').hidden,false);
      assert.equal(node('ramboot-address').value,'0x89000000');
      assert.match(node('target-value').textContent,/0x89000000/);
      context.selectedFile.name='q1000k-sysupgrade.itb';
      await context.upload();assert.equal(requests.length,4);
      context.selectedFile.name='q1000k-initramfs-recovery.itb';
      context.selectedFile.size=256*1024*1024+1;
      await context.upload();assert.equal(requests.length,4);
      context.selectedFile.size=2000000;
      const before=confirmations;
      await context.upload();assert.equal(requests[4].url,'/upload/initramfs?addr=0x89000000');
      assert.equal(node('ramboot-address').disabled,true);
      assert.equal(confirmations,before);
      xhrs[4].events.load();
      assert.equal(node('state').textContent,'Boot requested');
      assert.equal(context.pollTimer,null);
      assert.doesNotMatch(node('status-message').textContent,/erase|write|restarts in/);
      context.setBusy(false);
      node('ramboot-address').value='0x90000000';context.validateFile();
      await context.upload();assert.equal(requests[5].url,'/upload/initramfs?addr=0x90000000');
      xhrs[5].status=400;xhrs[5].events.load();
      assert.equal(node('state').textContent,'Rejected');
      assert.equal(context.busy,false);
      assert.equal(node('ramboot-address').disabled,false);
      for(const address of ['', '89000000', '0x84000000', '0x881ff000', '0x89000001',
                           '0xa0000000', '0x9ef00000', '0x9ee00000', '0x100000000',
                           '0x89000000&addr=0x90000000', '0x8900000g']){
        node('ramboot-address').value=address;context.validateFile();
        assert.equal(node('upload-button').disabled,true,address);
        assert.match(node('ramboot-address-help').className,/warning/);
        const count=requests.length;await context.upload();assert.equal(requests.length,count);
      }
      node('ramboot-address').value='0X88200000';context.validateFile();
      assert.equal(node('upload-button').disabled,false);
      node('ramboot-address').value='0x90000000';
      await context.upload();xhrs[6].events.error();
      assert.equal(node('state').textContent,'Unconfirmed');
      assert.equal(context.busy,false);
      context.setTarget('firmware');
      assert.equal(node('ramboot-address-block').hidden,true);
      assert.equal(node('row-erase').hidden,false);
      assert.equal(node('row-write').hidden,false);
      context.setTarget('initramfs');
      assert.equal(node('ramboot-address').value,'0x90000000');
    }else{
      assert.equal(context.activeTarget,'firmware');
    }
  } else {
    assert.equal(context.q1000kInstaller,false);
    context.setTarget('recovery');assert.equal(context.activeTarget,'firmware');
    context.setTarget('initramfs');assert.equal(context.activeTarget,'firmware');
    context.setTarget('uboot');assert.equal(node('self-write-button').hidden,false);
    assert.equal(node('uboot-mode-block').hidden,true);
    assert.equal(node('upload-button').textContent,'Upload and Update');
  }
}
(async()=>{await scenario('Q1000K');await scenario('Q1000K',false);await scenario('XG2010G');console.log('PASS: installer confirmation, RAM boot routing/results, capability gates and other-board UI');})().catch(e=>{console.error(e);process.exitCode=1});
