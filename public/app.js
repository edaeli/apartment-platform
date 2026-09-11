const grid = document.querySelector('#listings');
const template = document.querySelector('#listing-template');
const message = document.querySelector('#message');
const count = document.querySelector('#catalog-count');
const retry = document.querySelector('#retry');
const tabs = [...document.querySelectorAll('.tab')];
const numberFormat = new Intl.NumberFormat('ru-RU');
let selectedType = '';
let currentRequest;

function renderListing(item) {
  const card = template.content.cloneNode(true);
  const image = card.querySelector('img');
  image.src = item.photos[0]?.url || '/images/placeholder.svg';
  image.alt = `${item.photos[0]?.caption || 'Недвижимость'} — демонстрационная фотография`;
  image.addEventListener('error', () => { image.src = '/images/placeholder.svg'; }, { once: true });
  card.querySelector('.deal-badge').textContent = item.deal_type === 'rent' ? 'В аренду' : 'На продажу';
  card.querySelector('.photo-count').textContent = `${item.photos.length} фото`;
  card.querySelector('.district').textContent = item.district;
  card.querySelector('h3').textContent = item.kind === 'house'
    ? `Дом · ${numberFormat.format(item.area)} м²`
    : `${item.rooms}-комнатная квартира · ${numberFormat.format(item.area)} м²`;
  card.querySelector('.address').textContent = item.address;
  card.querySelector('.features').textContent = `${item.rooms} комн.  ·  ${numberFormat.format(item.area)} м²  ·  ${item.kind === 'house' ? 'Отдельный дом' : `${item.floor} этаж`}`;
  card.querySelector('.description').textContent = item.description;
  const price = card.querySelector('.price');
  price.textContent = `${numberFormat.format(item.price)} ֏`;
  const period = document.createElement('span');
  period.textContent = item.deal_type === 'rent' ? ' / месяц' : ' за объект';
  price.append(period);
  return card;
}

async function loadListings() {
  currentRequest?.abort();
  const controller = new AbortController();
  currentRequest = controller;
  grid.setAttribute('aria-busy', 'true');
  grid.replaceChildren();
  message.hidden = false;
  message.textContent = 'Загружаем объявления из каталога…';
  count.textContent = 'Загрузка…';
  retry.hidden = true;
  try {
    // Интерфейс получает JSON от C++; SQL и выборка находятся на сервере.
    const query = selectedType ? `?type=${encodeURIComponent(selectedType)}` : '';
    const response = await fetch(`/api/listings${query}`, { signal: controller.signal });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (controller.signal.aborted) return;
    const fragment = document.createDocumentFragment();
    data.items.forEach(item => fragment.append(renderListing(item)));
    grid.replaceChildren(fragment);
    count.textContent = `Найдено объектов: ${data.count}`;
    message.textContent = 'Пока нет доступных объявлений в этом разделе.';
    message.hidden = data.items.length > 0;
  } catch (error) {
    if (controller.signal.aborted) return;
    message.textContent = 'Не удалось загрузить каталог. Проверьте, что сервер запущен, и попробуйте снова.';
    count.textContent = 'Каталог недоступен';
    retry.hidden = false;
    console.error('Ошибка загрузки каталога:', error);
  } finally {
    if (currentRequest === controller) grid.setAttribute('aria-busy', 'false');
  }
}

tabs.forEach(tab => tab.addEventListener('click', () => {
  selectedType = tab.dataset.type;
  tabs.forEach(button => {
    const active = button === tab;
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', String(active));
  });
  loadListings();
}));
retry.addEventListener('click', loadListings);
loadListings();
