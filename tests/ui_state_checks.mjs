// Проверка логики с заглушками, НЕ браузер: нет layout, cookies и настоящего DOM.
// Запуск при наличии Node.js: node tests/ui_state_checks.mjs
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
const root = new URL('../public/', import.meta.url);
const read = name => readFile(new URL(name, root), 'utf8');
class Element {
  children = []; dataset = {}; attributes = {}; listeners = {}; hidden = false; textContent = '';
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
  setAttribute(name, value) { this.attributes[name] = value; }
  addEventListener(name, handler) { this.listeners[name] = handler; }
  focus() {}
  scrollIntoView() {}
}
const all = [], ids = new Map();
const make = () => { const node = new Element(); all.push(node); return node; };
const node = id => { if (!ids.has(id)) ids.set(id, make()); return ids.get(id); };
let calls = 0, resolveMutation;
const context = vm.createContext({
  document: {createElement: make, querySelector: node,
    querySelectorAll: selector => all.filter(item => String(item.dataset.favoriteId) === selector.match(/"(\d+)"/)[1])},
  window: {dispatchEvent() {}, addEventListener() {}}, CustomEvent: class {},
  Auth: {state: {user: {id: 1}}, mutate: () => { ++calls; return new Promise(resolve => { resolveMutation = resolve; }); }, post: async () => ({id:10,listing_id:1,status:'active',message: 'Бронирование создано'})},
  authReady: Promise.resolve(), loginAddress: () => '/login',
  location: {pathname:'/listings/1', search:'', origin:'http://localhost:8080'},
  URL, URLSearchParams, console
});
const run = code => vm.runInContext(code, context);
for (const name of ['common.js', 'personal-client.js']) run(await read(name));
run('var first = favoriteControl({id:1,is_favorite:false}); var second = favoriteControl({id:1,is_favorite:false});');
run('syncFavorites([{id:1,is_favorite:true}], favoriteRevision)');
assert.equal(run('first.children[0].attributes["aria-pressed"]'), 'true');
assert.equal(run('second.children[0].attributes["aria-pressed"]'), 'true');
console.log('PASS 1: свежий ответ обновляет обе кнопки одного объявления');
run('var before = favoriteRevision');
const mutation = run('first.children[0].listeners.click()');
await run('second.children[0].listeners.click()');
assert.equal(calls, 1);
assert.equal(run('second.children[0].disabled'), true);
run('syncFavorites([{id:1,is_favorite:false}], favoriteRevision)');
assert.equal(run('favoriteStates.get(1)'), true);
resolveMutation({is_favorite: false}); await mutation;
assert.equal(run('first.children[0].disabled'), false);
console.log('PASS 2: повторное нажатие и чтение во время изменения защищены');
run('syncFavorites([{id:1,is_favorite:true}], before)');
assert.equal(run('favoriteStates.get(1)'), false);
run('syncFavorites([{id:1,is_favorite:true}], favoriteRevision)');
assert.equal(run('favoriteStates.get(1)'), true);
console.log('PASS 3: запоздалое чтение не отменяет результат, новое чтение принимается');
context.fixture = {id:1,status:'available',deal_type:'rent',kind:'apartment',rooms:2,area:60,price:200000,
  photos:[{url:'/one.jpg',caption:'Первое'},{url:'/two.jpg',caption:'Второе'}],is_favorite:true};
context.fetch = async () => ({ok:true,json:async () => context.fixture});
run(await read('listing.js'));
// Дожидаемся цепочки начальной загрузки без таймеров и сети.
for (let i=0;i<12;i++) await Promise.resolve();
node('#thumbnails').children[1].listeners.click();
await run('loadDetail()');
assert.equal(node('#main-photo').src, '/two.jpg');
assert.equal(node('#thumbnails').children[1].attributes['aria-pressed'], 'true');
console.log('PASS 4: повторная загрузка сохраняет выбранную фотографию');
context.fetch = async () => { throw new Error('offline'); };
await node('#book-button').listeners.click();
assert.equal(node('#booking-message').textContent, 'Бронирование создано');
assert.equal(node('#booking-account').hidden, false);
assert.equal(node('#detail-retry').hidden, false);
const html = await read('listing.html');
assert.ok(html.indexOf('id="booking-message"') < html.indexOf('<article'));
assert.ok(html.indexOf('id="booking-account"') < html.indexOf('<article'));
console.log('PASS 5: результат бронирования вне скрываемого блока после ошибки обновления');
const img = make(); context.img = img;
run('showPhoto(img, null)'); assert.equal(img.src, '/images/placeholder.svg');
run('showPhoto(img, {url:"/missing.jpg",caption:"Фото"})'); img.onerror();
assert.equal(img.src, '/images/placeholder.svg'); assert.equal(img.onerror, null);
console.log('PASS 6: отсутствующее и не загрузившееся фото используют заглушку');
run("fixture.renovation = 'designer'; renderDetail(fixture)");
let repairIndex = node('#characteristics').children.findIndex(item => item.textContent === 'Ремонт');
assert.ok(repairIndex >= 0);
assert.equal(node('#characteristics').children[repairIndex + 1].textContent, 'Дизайнерский');
run("fixture.renovation = 'unspecified'; renderDetail(fixture)");
assert.equal(node('#characteristics').children[repairIndex + 1].textContent, 'Не указано');
run('delete fixture.renovation; renderDetail(fixture)');
assert.equal(node('#characteristics').children.some(item => item.textContent === 'Ремонт'), false);
console.log('PASS 7: поле ремонта из API отображается по-русски; старый API не даёт ложного состояния');
for (const name of ['app.js','auth-client.js','auth-page.js','bookings.js','common.js','listing.js','personal-client.js','personal.js','recommendations.js']) new vm.Script(await read(name), {filename:name});
console.log('7/7 проверок логики и синтаксис 9 JS-файлов: успешно. Браузер не использован.');
