// SPDX-License-Identifier: GPL-2.0+
// Unit-test UI routing and destructive-install confirmation with inert DOM/XHR stubs.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../lib/lwip/httpd/htdocs/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
assert(html.includes('[hidden]{display:none!important}'));
async function scenario(board) {
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
  const prompts = [];
  let confirmations=0, accept=false;
  const context = vm.createContext({
    document:{getElementById:node,querySelectorAll(){return []},createElement:node,body:node('body')},
    location:{protocol:'http:'},
    window:{confirm(message){confirmations++;prompts.push(message);return accept}},
    fetch:async()=>({json:async()=>({board,chainloader_update:true,firmware_mode:'ubi'})}),
    setInterval(){return 1},clearInterval(){},setTimeout(){},
    XMLHttpRequest:function(){this.upload={addEventListener(){}};this.addEventListener=()=>{};
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
  } else {
    assert.equal(context.q1000kInstaller,false);
    context.setTarget('recovery');assert.equal(context.activeTarget,'firmware');
    context.setTarget('uboot');assert.equal(node('self-write-button').hidden,false);
    assert.equal(node('uboot-mode-block').hidden,true);
    assert.equal(node('upload-button').textContent,'Upload and Update');
  }
}
(async()=>{await scenario('Q1000K');await scenario('XG2010G');console.log('PASS: installer confirmation, target routes, preservation text and other-board UI');})().catch(e=>{console.error(e);process.exitCode=1});
