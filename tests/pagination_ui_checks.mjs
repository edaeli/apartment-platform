// Настоящие app.js и DOM index.html; fetch, история и прокрутка подставлены.
// НЕ браузер: геометрия/фактический scrollY не проверяются.
// python3 tests/prepare_dom_tests.py && node tests/pagination_ui_checks.mjs
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
import {parseHTML} from '../build/test-deps/linkedom-worker.mjs';
const source=await readFile(new URL('../public/app.js',import.meta.url),'utf8');
const html=await readFile(new URL('../public/index.html',import.meta.url),'utf8');
async function page(search='') {
  const {document,window:dom}=parseHTML(html), events={}, scrolls=[], requests=[];
  const target=document.querySelector('#catalog .tabs');
  target.scrollIntoView=options=>scrolls.push({target,...options});
  const location={search,origin:'http://localhost',pathname:'/'};
  const form=document.querySelector('#filters');
  form.elements={namedItem:name=>form.querySelector(`[name="${name}"]`)};
  // LinkeDOM не реализует все API HTML-форм; дополняем только используемые здесь.
  for(const select of form.querySelectorAll('select')) {
    Object.defineProperty(select,'value',{value:'',writable:true});
    select.add=option=>select.append(option);
  }
  function Option(text,value){const option=document.createElement('option');option.textContent=text;option.value=value;return option;}
  const context=vm.createContext({document,location,Option,URLSearchParams,AbortController,
    history:{pushState(_state,_title,url){location.search=new URL(url,location.origin).search;}},
    window:{addEventListener:(name,handler)=>{events[name]=handler;},scrollTo:()=>{throw new Error('Unexpected fixed document coordinate');}},
    authReady:Promise.resolve(),favoriteRevision:0,syncFavorites(){},listingCard(){throw new Error('Unexpected card');},
    numberFormat:new Intl.NumberFormat('ru'),console,
    fetch:(url,options)=>new Promise(resolve=>requests.push({url,options,resolve}))});
  const run=code=>vm.runInContext(code,context);
  const flush=async()=>{for(let i=0;i<20;i++)await Promise.resolve();};
  const answer=async(status=200)=>{
    const request=requests.shift();assert.ok(request,'Expected request');
    const page=Number(new URL(request.url,location.origin).searchParams.get('page')||1);
    request.resolve({ok:status===200,status,json:async()=>({items:[],districts:['Кентрон'],
      page,total_pages:4,page_size:24,count:24,total:96,has_previous:page>1,has_next:page<4,error:'Ошибка теста'})});
    await flush();return request;
  };
  vm.runInContext(source,context);await flush();await answer();
  const click=async selector=>{
    document.querySelector(selector).dispatchEvent(new dom.Event('click',{bubbles:true,cancelable:true}));
    await flush();
  };
  return {document,context,location,scrolls,requests,run,flush,answer,click,events};
}
for(const search of ['', '?type=rent&district=%D0%9A%D0%B5%D0%BD%D1%82%D1%80%D0%BE%D0%BD&min_price=100000&max_price=500000&rooms=2&sort=area_desc']) {
  const p=await page(search);assert.equal(p.scrolls.length,0,'Initial load must not scroll');
  const filters=new URLSearchParams(search);
  for(const [selector,expectedPage] of [['#next-page',2],['#previous-page',1],['#page-links a:last-child',4]]) {
    const before=p.scrolls.length;await p.click(selector);
    assert.equal(p.scrolls.length,before,'No scrolling before successful response');
    const query=new URLSearchParams(p.location.search);assert.equal(query.get('page'),String(expectedPage));
    for(const [key,value] of filters)assert.equal(query.get(key),value);
    await p.answer();assert.equal(p.scrolls.length,before+1);
    assert.deepEqual(p.scrolls.at(-1),{target:p.document.querySelector('#catalog .tabs'),block:'start',inline:'nearest',behavior:'instant'});
  }
}
console.log('PASS 1: вперёд/назад/номер — переключатели типа сделки после успеха; с фильтрами и без, URL сохранён');
const p=await page('?page=2&sort=price_desc');
await p.click('#next-page');await p.answer(500);assert.equal(p.scrolls.length,0);
assert.equal(p.document.querySelector('#retry').hidden,false);
await p.click('#retry');await p.answer();assert.equal(p.scrolls.length,0);
console.log('PASS 2: ошибка и обычный повтор загрузки не прокручивают документ');
const background=p.run('loadListings()');await p.flush();await p.answer();await background;
p.events.popstate();await p.flush();await p.answer();assert.equal(p.scrolls.length,0);
assert.equal(p.context.history.scrollRestoration,undefined);
console.log('PASS 3: начальная загрузка, фоновое чтение и popstate не меняют восстановление позиции');
await p.click('#previous-page');
const superseding=p.run('loadListings()');await p.flush();
const cancelled=await p.answer();assert.equal(cancelled.options.signal.aborted,true);
assert.equal(p.scrolls.length,0);await p.answer();await superseding;assert.equal(p.scrolls.length,0);
console.log('PASS 4: отменённый переход не прокручивает после запоздалого ответа');
console.log('4/4 групп. Проверены обработчики, ориентир и параметры scrollIntoView, не реальные клики/scrollY браузера.');
