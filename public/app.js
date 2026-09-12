const grid = document.querySelector('#listings');
const template = document.querySelector('#listing-template');
const message = document.querySelector('#message');
const count = document.querySelector('#catalog-count');
const retry = document.querySelector('#retry');
const form = document.querySelector('#filters');
const tabs = [...document.querySelectorAll('.tab')];
const pagination = document.querySelector('#pagination');
const firstPage = document.querySelector('#first-page');
let currentRequest;

function restoreForm() {
  const params = new URLSearchParams(location.search);
  for (const name of ['type', 'district', 'min_price', 'max_price', 'rooms', 'sort']) {
    const control = form.elements.namedItem(name);
    const value = params.get(name) ?? (name === 'sort' ? 'price_asc' : '');
    if (control.tagName === 'SELECT' && ![...control.options].some(option => option.value === value)) {
      control.add(new Option(value, value));
    }
    control.value = value;
  }
  tabs.forEach(tab => {
    const active = tab.dataset.type === form.elements.namedItem('type').value;
    tab.classList.toggle('active', active);
    tab.setAttribute('aria-pressed', String(active));
  });
}
function pageUrl(page) {
  const params = new URLSearchParams(location.search);
  params.set('page', String(page));
  return `/?${params}#catalog`;
}
function navigate(url) {
  history.pushState(null, '', url);
  restoreForm();
  loadListings();
}
function renderListing(item) {
  const card = template.content.cloneNode(true);
  showPhoto(card.querySelector('img'), item.photos[0]);
  const href = `/listings/${item.id}?return_to=${encodeURIComponent(`/${location.search}#catalog`)}`;
  card.querySelector('.cover-link').href = href;
  const link = card.querySelector('.listing-link');
  link.href = href;
  link.textContent = listingTitle(item);
  card.querySelector('.deal-badge').textContent = item.deal_type === 'rent' ? 'В аренду' : 'На продажу';
  card.querySelector('.photo-count').textContent = item.photos.length ? `${item.photos.length} фото` : 'Нет фото';
  card.querySelector('.district').textContent = item.district;
  card.querySelector('.address').textContent = item.address;
  card.querySelector('.features').textContent = `${item.rooms} комн.  ·  ${numberFormat.format(item.area)} м²  ·  ${item.kind === 'house' ? 'Отдельный дом' : `${item.floor} этаж`}`;
  card.querySelector('.description').textContent = item.description;
  showPrice(card.querySelector('.price'), item);
  return card;
}
function renderPagination(data) {
  pagination.hidden = data.total_pages <= 1 || data.count === 0;
  const previous = document.querySelector('#previous-page');
  const next = document.querySelector('#next-page');
  previous.hidden = !data.has_previous;
  previous.href = pageUrl(data.page - 1);
  next.hidden = !data.has_next;
  next.href = pageUrl(data.page + 1);
  const links = document.querySelector('#page-links');
  links.replaceChildren();
  const pages = new Set([1, data.total_pages]);
  for (let page = Math.max(1, data.page - 2); page <= Math.min(data.total_pages, data.page + 2); page++) pages.add(page);
  let previousNumber = 0;
  [...pages].sort((a, b) => a - b).forEach(page => {
    if (page > previousNumber + 1) {
      const gap = document.createElement('span');
      gap.textContent = '…';
      links.append(gap);
    }
    const link = document.createElement('a');
    link.className = 'page-link';
    link.href = pageUrl(page);
    link.textContent = page;
    link.setAttribute('aria-label', `Страница ${page}`);
    if (page === data.page) link.setAttribute('aria-current', 'page');
    links.append(link);
    previousNumber = page;
  });
  document.querySelector('#page-summary').textContent = `Страница ${data.page} из ${data.total_pages} · по ${data.page_size} объявлений`;
}
async function loadListings() {
  currentRequest?.abort();
  const controller = new AbortController();
  currentRequest = controller;
  grid.setAttribute('aria-busy', 'true');
  grid.replaceChildren();
  pagination.hidden = firstPage.hidden = retry.hidden = true;
  message.hidden = false;
  message.textContent = 'Загружаем объявления…';
  count.textContent = 'Загрузка…';
  try {
    const response = await fetch(`/api/listings${location.search}`, { signal: controller.signal });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `Ошибка HTTP ${response.status}`);
    if (controller.signal.aborted) return;
    const district = form.elements.namedItem('district');
    const selected = district.value;
    district.replaceChildren(new Option('Все районы', ''));
    data.districts.forEach(name => district.add(new Option(name, name)));
    if (selected && !data.districts.includes(selected)) district.add(new Option(selected, selected));
    district.value = selected;
    const fragment = document.createDocumentFragment();
    data.items.forEach(item => fragment.append(renderListing(item)));
    grid.replaceChildren(fragment);
    count.textContent = `Найдено объявлений: ${numberFormat.format(data.total)}`;
    message.hidden = data.count > 0;
    message.textContent = data.total > 0
      ? 'На этой странице объявлений нет. Перейдите на первую страницу.'
      : 'Ничего не найдено. Измените условия или сбросьте фильтры.';
    firstPage.hidden = !(data.total > 0 && data.count === 0);
    firstPage.href = pageUrl(1);
    renderPagination(data);
  } catch (error) {
    if (controller.signal.aborted) return;
    message.textContent = `Не удалось загрузить каталог. ${error.message}`;
    count.textContent = 'Каталог недоступен';
    retry.hidden = false;
  } finally {
    if (currentRequest === controller) grid.setAttribute('aria-busy', 'false');
  }
}
form.addEventListener('submit', event => {
  event.preventDefault();
  const params = new URLSearchParams();
  for (const [key, value] of new FormData(form)) if (value !== '') params.set(key, value);
  const size = new URLSearchParams(location.search).get('page_size');
  if (size) params.set('page_size', size);
  // Применение новых условий всегда возвращает на первую страницу.
  navigate(`/?${params}#catalog`);
});
tabs.forEach(tab => tab.addEventListener('click', () => {
  form.elements.namedItem('type').value = tab.dataset.type;
  form.requestSubmit();
}));
form.elements.namedItem('sort').addEventListener('change', () => form.requestSubmit());
document.querySelector('#reset-filters').addEventListener('click', () => navigate('/#catalog'));
pagination.addEventListener('click', event => {
  const link = event.target.closest('a');
  if (!link || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
  event.preventDefault();
  navigate(link.href);
  document.querySelector('#catalog').scrollIntoView();
});
retry.addEventListener('click', loadListings);
window.addEventListener('popstate', () => { restoreForm(); loadListings(); });
restoreForm();
loadListings();
