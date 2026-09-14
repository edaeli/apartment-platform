// Реальные скрипты страницы и DOM из listing.html, заглушка fetch. НЕ браузерная проверка.
// node tests/booking_ui_checks.mjs
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
const read = name => readFile(new URL('../public/' + name, import.meta.url), 'utf8');
import {parseHTML} from '../build/test-deps/linkedom-worker.mjs';
const source = (await Promise.all(['common.js','auth-client.js','personal-client.js','listing.js'].map(read))).join('\n');
const html = await read('listing.html');
function visible(element) {
  assert.ok(element?.isConnected, 'Уведомление должно оставаться в документе');
  for (let parent=element; parent; parent=parent.parentElement) {
    assert.equal(parent.hidden, false, `Скрытый предок: ${parent.id || parent.tagName}`);
    assert.notEqual(parent.getAttribute('aria-hidden'), 'true');
    assert.notEqual(parent.style.display, 'none');
    assert.notEqual(parent.style.visibility, 'hidden');
  }
}
const conflictText='Не удалось забронировать: это жильё уже занято. Выберите другое объявление';
const response=(status,body,bad=false)=>({status,ok:status>=200&&status<300,json:async()=>{
  if(bad)throw new SyntaxError('invalid JSON'); return structuredClone(body);
}});
async function page(userId, post, failRefresh=false, initialStatus='available') {
  const {document, HTMLElement}=parseHTML(html), events={};
  const node=selector=>document.querySelector(selector);
  // DOM настоящий; геометрии/отрисовки в LinkeDOM нет. Эти методы только наблюдаем.
  HTMLElement.prototype.focus=function(){this.focusCount=(this.focusCount||0)+1;};
  HTMLElement.prototype.scrollIntoView=function(){this.scrollCount=(this.scrollCount||0)+1;};
  let clicked;
  const button=node('#book-button');
  const add=button.addEventListener.bind(button);
  button.addEventListener=(type, handler)=>add(type, event=>{clicked=handler(event);});
  const item={id:1,status:initialStatus,deal_type:'rent',kind:'apartment',rooms:2,area:60,price:200000,photos:[],is_favorite:false};
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
    if(posted&&failRefresh===404)return response(404,{error:'Не найдено'});
    if(posted&&failRefresh)throw new TypeError('offline after action');
    return response(200,item);
  };
  const context=vm.createContext({document,
    window:{addEventListener:(name,fn)=>{events[name]=fn;},dispatchEvent(){}},
    location:{pathname:'/listings/1',search:'',origin:'http://localhost',reload(){},replace(){}},
    fetch, URL, URLSearchParams,console});
  vm.runInContext(source,context);
  await vm.runInContext('loadDetail()',context);
  return {node,context,item,setRefreshFailure:value=>{failRefresh=value;},calls:()=>calls,
    click:()=>{button.dispatchEvent(new document.defaultView.Event('click'));return clicked;},
    focus:()=>events.focus(), refresh:()=>vm.runInContext('loadDetail()',context)};
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
assert.equal(loser.node('#booking-feedback').getAttribute('role'),'alert');
assert.ok(loser.node('#booking-feedback').focusCount >= 1);
assert.ok(loser.node('#booking-feedback').scrollCount >= 1);
console.log('PASS 1: два обработчика fetch/JSON — успех только победителю, явный 409 проигравшему');
const conflictNode=loser.node('#booking-feedback');
function conflictPersists() {
  assert.equal(loser.node('#booking-feedback'),conflictNode);
  assert.equal(conflictNode.dataset.outcome,'error');
  assert.equal(loser.node('#booking-message').textContent,conflictText);
  assert.equal(loser.node('#booking-account').hidden,true);
  visible(loser.node('#booking-message'));
}
conflictPersists();
await loser.refresh(); conflictPersists();
await loser.focus(); conflictPersists();
await vm.runInContext('Auth.load().then(() => renderBookingAction())',loser.context);
conflictPersists();
await loser.click(); conflictPersists();
assert.equal(loser.node('#booking-feedback').parentElement,loser.node('#booking-action'));
assert.equal(loser.node('#booking-controls').contains(loser.node('#booking-feedback')),false);
assert.equal(loser.node('#booking-message').textContent,conflictText);
assert.equal(loser.node('#booking-account').hidden,true);
assert.equal(loser.calls(),1);
console.log('PASS 2: сообщение сохраняется после обновлений; повторного POST/успеха нет');
const offline=await page(3,()=>response(409,{error:'Это жильё уже забронировали'}),true);
await offline.click();
assert.equal(offline.node('#booking-message').textContent,conflictText);
assert.equal(offline.node('#book-button').hidden,true);
assert.equal(offline.node('#booking-account').hidden,true);
visible(offline.node('#booking-message'));
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
// Изменение сервера между открытием страницы и фокусом второго окна: POST ещё не было.
const stale=await page(6,()=>{throw new Error('POST не должен вызываться');});
stale.item.status='reserved'; await stale.focus();
const warningText='Это жильё только что забронировали. Выберите другое объявление';
assert.equal(stale.node('#booking-message').textContent,warningText);
assert.equal(stale.node('#booking-feedback').dataset.outcome,'warning');
assert.equal(stale.node('#booking-feedback').getAttribute('role'),'status');
assert.equal(stale.node('#booking-icon').textContent,'⚠');
assert.ok(stale.node('#booking-feedback').scrollCount>0);
await stale.focus(); await stale.refresh();
assert.equal(stale.node('#booking-message').textContent,warningText);
visible(stale.node('#booking-message'));
// Событие уже могло попасть в очередь, хотя фоновый GET успел скрыть кнопку.
await stale.click();
assert.equal(stale.calls(),0);
assert.equal(stale.node('#booking-message').textContent,conflictText);
await stale.refresh(); await stale.focus();
visible(stale.node('#booking-message'));
assert.equal(stale.node('#booking-account').hidden,true);
console.log('PASS 6: фоновое reserved без POST объяснено; ранний выход после нажатия даёт устойчивое уведомление');
for (const status of ['reserved','closed']) {
  const initial=await page(7,()=>{throw new Error('Unexpected POST');},false,status);
  await initial.refresh();
  visible(initial.node('#booking-message'));
  assert.equal(initial.node('#booking-feedback').dataset.outcome,'info');
  assert.equal(initial.node('#booking-message').textContent,'Объявление недоступно для нового бронирования');
  assert.equal(initial.node('#book-button').hidden,true);
  assert.equal(initial.node('#booking-account').hidden,true);
  assert.equal(initial.calls(),0);
}
console.log('PASS 7: открытие/обновление недоступного объявления — нейтральное пояснение, без ложного успеха или проигрыша');
const missing=await page(8,item=>{item.status='reserved';return response(409,{error:'Это жильё уже забронировали'});},404);
await missing.click();
const preserved=missing.node('#booking-feedback');
assert.equal(missing.node('#listing-detail').hidden,true);
visible(missing.node('#booking-message'));
assert.equal(missing.node('#booking-message').textContent,conflictText);
missing.setRefreshFailure(false); await missing.refresh();
assert.equal(missing.node('#booking-feedback'),preserved);
assert.equal(preserved.parentElement,missing.node('#booking-action'));
assert.equal(missing.node('#booking-account').hidden,true);
visible(missing.node('#booking-message'));
console.log('PASS 8: результат переживает 404 скрывающий article, повторный GET возвращает тот же узел рядом с действием');
// Проверяем, что сам контроль предков действительно обнаруживает скрытие.
loser.node('#listing-detail').hidden=true;
assert.throws(()=>visible(loser.node('#booking-message')),/Скрытый предок/);
loser.node('#listing-detail').hidden=false;
visible(winner.node('#booking-message'));
visible(loser.node('#booking-message'));
const successText=winner.node('#booking-message').textContent;
await winner.focus(); await winner.refresh();
assert.equal(winner.node('#booking-feedback').dataset.outcome,'success');
assert.equal(winner.node('#booking-message').textContent,successText);
assert.equal(winner.node('#booking-unavailable'),null);
assert.equal(winner.node('#booking-account').hidden,false);
let finishPost;
const delayed=await page(9,()=>new Promise(resolve=>{finishPost=resolve;}));
// GET стартует до POST, но его ответ приходит уже во время бронирования.
const normalFetch=delayed.context.fetch;
let resumeGet,signalGet;
const getStarted=new Promise(resolve=>{signalGet=resolve;});
delayed.context.fetch=(path,options)=>path==='/api/listings/1'
  ? new Promise(resolve=>{resumeGet=()=>resolve(response(200,{...delayed.item,status:'reserved'}));signalGet();})
  : normalFetch(path,options);
const inFlightGet=delayed.refresh(); await getStarted;
const pendingClick=delayed.click();
resumeGet(); await inFlightGet;
delayed.context.fetch=normalFetch;
assert.equal(delayed.node('#booking-feedback').dataset.outcome,'pending');
delayed.item.status='reserved';
await delayed.focus(); await delayed.refresh();
assert.equal(delayed.node('#booking-feedback').dataset.outcome,'pending');
finishPost(response(201,{id:99,listing_id:1,status:'active'}));
await pendingClick; await delayed.focus();
assert.equal(delayed.node('#booking-feedback').dataset.outcome,'success');
assert.equal(delayed.node('#booking-unavailable'),null);
visible(delayed.node('#booking-message'));
console.log('PASS 9: успех не заменяется фоновым предупреждением; во время POST ожидается его результат, дублей нет');
console.log('9/9 групп: реальные HTML/DOM и обработчики. CSS layout и реальные клики в браузере НЕ проверены.');
// Оценка проходит через настоящий renderDetail, не через отдельный форматтер.
for (const deal of ['rent','sale']) {
  vm.runInContext(`currentListing.deal_type='${deal}'; currentListing.price_estimate={status:'available',amount_amd:123456.75}; renderDetail(currentListing)`,winner.context);
  const text=winner.node('#model-price').textContent;
  assert.ok(text.startsWith('Учебная оценка модели:'));
  assert.ok(text.endsWith(deal==='rent'?'в месяц':'за объект'));
  assert.ok(text.includes('123') && text.includes('457'));
  visible(winner.node('#model-price'));
}
for(const value of [undefined, {status:'unavailable'}, {status:'available',amount_amd:NaN},
  {status:'available',amount_amd:Infinity},{status:'available',amount_amd:0},
  {status:'available',amount_amd:'<script>alert(1)</script>'},{status:'available',amount_amd:1e20}]) {
  winner.context.estimateFixture=value;
  vm.runInContext('currentListing.price_estimate=estimateFixture; renderDetail(currentListing)',winner.context);
  assert.equal(winner.node('#model-price').textContent,'Учебная оценка модели недоступна');
}
assert.ok(winner.node('.model-estimate').textContent.includes('Это не рыночная оценка'));
console.log('PASS 10: реальная страница — оценка обеих сделок, округление, пояснение, отсутствие/невалидные числа/старый API');
console.log('10/10 групп с ML UI. Визуальное отображение в браузере не проверено.');
