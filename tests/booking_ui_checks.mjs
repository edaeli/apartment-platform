// Реальные auth-client.js + listing.js, заглушки fetch/DOM. НЕ браузерная проверка.
// node tests/booking_ui_checks.mjs
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
const read = name => readFile(new URL('../public/' + name, import.meta.url), 'utf8');
class Element {
  children=[];dataset={};attributes={};listeners={};hidden=false;textContent='';focusCount=0;scrollCount=0;
  append(...nodes){this.children.push(...nodes);}
  replaceChildren(...nodes){this.children=nodes;}
  setAttribute(k,v){this.attributes[k]=v;}
  addEventListener(k,v){this.listeners[k]=v;}
  focus(){++this.focusCount;}
  scrollIntoView(){++this.scrollCount;}
}
const source = (await Promise.all(['common.js','auth-client.js','personal-client.js','listing.js'].map(read))).join('\n');
const html = await read('listing.html');
assert.ok(html.indexOf('id="booking-feedback"') < html.indexOf('<article'));
assert.ok(html.indexOf('id="booking-account"') < html.indexOf('<article'));
assert.ok(html.indexOf('</section>',html.indexOf('id="booking-feedback"')) < html.indexOf('<article'));
const conflictText='Это жильё уже забронировали. Выберите другое объявление';
const response=(status,body,bad=false)=>({status,ok:status>=200&&status<300,json:async()=>{
  if(bad)throw new SyntaxError('invalid JSON'); return structuredClone(body);
}});
async function page(userId, post, failRefresh=false) {
  const elements=new Map(),events={};
  const node=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
  const item={id:1,status:'available',deal_type:'rent',kind:'apartment',rooms:2,area:60,price:200000,photos:[],is_favorite:false};
  let posted=false,calls=0;
  const fetch=async(path,options={})=>{
    if(path==='/api/auth/me')return response(200,{user:{id:userId,name:'Test'},csrf_token:'fixture-token'});
    if(path==='/api/listings/1/view')return response(200,{counted:true});
    if(path==='/api/listings/1/book'){
      ++calls;posted=true;
      assert.equal(options.method,'POST');assert.equal(options.headers['X-CSRF-Token'],'fixture-token');
      return post(item);
    }
    assert.equal(path,'/api/listings/1');
    if(posted&&failRefresh)throw new TypeError('offline after action');
    return response(200,item);
  };
  const context=vm.createContext({document:{querySelector:node,querySelectorAll:()=>[],createElement:()=>new Element()},
    window:{addEventListener:(name,fn)=>{events[name]=fn;},dispatchEvent(){}},
    location:{pathname:'/listings/1',search:'',origin:'http://localhost',reload(){},replace(){}},
    fetch, URL, URLSearchParams,console});
  vm.runInContext(source,context);
  await vm.runInContext('loadDetail()',context);
  return {node,context,calls:()=>calls,click:()=>node('#book-button').listeners.click(),refresh:()=>vm.runInContext('loadDetail()',context)};
}
// Два независимых JS-контекста; точные 201/409 и тело отдельно подтверждены HTTP-тестом.
let arrived=0,release;
const barrier=new Promise(resolve=>{release=resolve;});
const arrive=async()=>{if(++arrived===2)release();await barrier;};
const winner=await page(1,async item=>{await arrive();item.status='reserved';return response(201,{id:42,listing_id:1,status:'active',message:'Бронирование создано. Оплата не производится'});});
const loser=await page(2,async item=>{await arrive();item.status='reserved';return response(409,{error:'Это жильё уже забронировали'});});
await Promise.all([winner.click(),loser.click()]);
assert.equal(winner.node('#booking-feedback').dataset.outcome,'success');
assert.equal(winner.node('#booking-account').hidden,false);
assert.equal(loser.node('#booking-feedback').dataset.outcome,'error');
assert.equal(loser.node('#booking-message').textContent,conflictText);
assert.equal(loser.node('#booking-account').hidden,true);
assert.equal(loser.node('#book-button').hidden,true);
assert.equal(loser.node('#detail-status').textContent,'Забронировано');
assert.equal(loser.node('#booking-feedback').hidden,false);
assert.equal(loser.node('#booking-feedback').attributes.role,'alert');
assert.ok(loser.node('#booking-feedback').focusCount >= 1);
assert.ok(loser.node('#booking-feedback').scrollCount >= 1);
console.log('PASS 1: два обработчика fetch/JSON — успех только победителю, явный 409 проигравшему');
await loser.refresh();await loser.refresh();await loser.click();
assert.equal(loser.node('#booking-message').textContent,conflictText);
assert.equal(loser.node('#booking-account').hidden,true);
assert.equal(loser.calls(),1);
console.log('PASS 2: сообщение сохраняется после обновлений; повторного POST/успеха нет');
const offline=await page(3,()=>response(409,{error:'Это жильё уже забронировали'}),true);
await offline.click();
assert.equal(offline.node('#booking-message').textContent,conflictText);
assert.equal(offline.node('#book-button').hidden,true);
assert.equal(offline.node('#booking-account').hidden,true);
console.log('PASS 3: 409 остаётся видимым и блокирует повтор даже при сбое чтения статуса');
for (const post of [()=>{throw new TypeError('offline');},()=>response(503,{error:'База временно занята'}),()=>response(500,null,true),()=>response(201,null,true),()=>response(201,{})]) {
  const errorPage=await page(4,post);await errorPage.click();
  assert.equal(errorPage.node('#booking-feedback').dataset.outcome,'error');
  assert.equal(errorPage.node('#booking-account').hidden,true);
  assert.ok(errorPage.node('#booking-message').textContent);
}
console.log('PASS 4: сеть, 503/500 и повреждённые подтверждения не выглядят как успех');
const bad409=await page(5,()=>response(409,null,true),true);await bad409.click();
assert.equal(bad409.node('#booking-message').textContent,conflictText);
console.log('PASS 5: HTTP 409 сохраняется даже при нечитаемом JSON');
console.log('5/5 групп регрессионных проверок. Layout и реальные клики не проверены.');
