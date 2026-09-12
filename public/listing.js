const detail = document.querySelector('#listing-detail');
const detailMessage = document.querySelector('#detail-message');
const detailRetry = document.querySelector('#detail-retry');
const id = location.pathname.split('/').filter(Boolean).at(-1);

// Возврат разрешён только в каталог нашего сайта, с его фильтрами и страницей.
try {
  const target = new URL(new URLSearchParams(location.search).get('return_to') || '/#catalog', location.origin);
  if (target.origin === location.origin && target.pathname === '/') {
    document.querySelector('#back-to-catalog').href = target.href;
  }
} catch { /* Некорректный return_to оставляет обычную ссылку на каталог. */ }

let currentListing;
let bookingPending = false;
let detailRequest = 0;
let viewSent = false;
let selectedPhotoUrl;
function renderDetail(item) {
  currentListing = item;
  renderBookingAction();
  // Ответ чтения мог прийти, пока пользователь уже нажал кнопку избранного.
  // Сохраняем саму кнопку и её сообщение до завершения действия.
  if (!favoritePending.has(item.id)) {
    document.querySelector('#detail-favorite').replaceChildren(favoriteControl(item));
  }
  document.title = `${listingTitle(item)} — Свой адрес`;
  document.querySelector('#detail-title').textContent = listingTitle(item);
  document.querySelector('#detail-address').textContent = item.address;
  document.querySelector('#detail-district').textContent = item.district;
  document.querySelector('#detail-description').textContent = item.description;
  document.querySelector('#detail-deal').textContent = item.deal_type === 'rent' ? 'Аренда' : 'Продажа';
  showPrice(document.querySelector('#detail-price'), item);
  const status = document.querySelector('#detail-status');
  status.textContent = { available: 'Доступно', reserved: 'Забронировано', closed: 'Закрыто' }[item.status] || item.status;
  status.dataset.status = item.status;
  const characteristics = document.querySelector('#characteristics');
  characteristics.replaceChildren();
  for (const [name, value] of [
    ['Объявление', `№ ${item.id}`], ['Тип объекта', item.kind === 'house' ? 'Дом' : 'Квартира'],
    ['Площадь', `${numberFormat.format(item.area)} м²`], ['Комнат', item.rooms],
    ['Этаж', item.kind === 'house' ? 'Объект целиком' : item.floor], ['Район', item.district]
  ]) {
    const term = document.createElement('dt');
    const definition = document.createElement('dd');
    term.textContent = name;
    definition.textContent = value;
    characteristics.append(term, definition);
  }
  const mainPhoto = document.querySelector('#main-photo');
  const selectedIndex = Math.max(0, item.photos.findIndex(photo => photo.url === selectedPhotoUrl));
  showPhoto(mainPhoto, item.photos[selectedIndex]);
  const thumbnails = document.querySelector('#thumbnails');
  thumbnails.replaceChildren();
  item.photos.forEach((photo, index) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'thumbnail';
    button.setAttribute('aria-label', `Показать фото ${index + 1}: ${photo.caption}`);
    button.setAttribute('aria-pressed', String(index === selectedIndex));
    const image = document.createElement('img');
    showPhoto(image, photo);
    button.append(image);
    button.addEventListener('click', () => {
      selectedPhotoUrl = photo.url;
      showPhoto(mainPhoto, photo);
      [...thumbnails.children].forEach(child => child.setAttribute('aria-pressed', String(child === button)));
    });
    thumbnails.append(button);
  });
}

async function loadDetail() {
  await authReady;
  if (bookingPending || favoritePending.has(Number(id))) return;
  const request = ++detailRequest;
  const revision = favoriteRevision;
  detail.hidden = detailRetry.hidden = true;
  detailMessage.hidden = false;
  detailMessage.textContent = 'Загружаем объявление…';
  detail.setAttribute('aria-busy', 'true');
  try {
    const response = await fetch(`/api/listings/${encodeURIComponent(id)}`);
    const item = await response.json();
    if (request !== detailRequest) return;
    if (!response.ok) {
      detailMessage.textContent = response.status === 404
        ? 'Объявление не найдено. Проверьте адрес или вернитесь в каталог.'
        : item.error || 'Не удалось загрузить объявление.';
      detailRetry.hidden = response.status < 500;
      return;
    }
    syncFavorites([item], revision);
    renderDetail(item);
    if (!viewSent && Auth.state?.user) {
      viewSent = true;
      Auth.post(`/api/listings/${encodeURIComponent(id)}/view`).catch(error => {
        document.querySelector('#view-message').textContent = 'Просмотр не сохранён. ' + error.message;
      });
    }
    detail.hidden = false;
    detailMessage.hidden = true;
  } catch {
    if (request !== detailRequest) return;
    detailMessage.textContent = 'Не удалось связаться с сервером. Попробуйте снова.';
    detailRetry.hidden = false;
  } finally {
    if (request === detailRequest) detail.setAttribute('aria-busy', 'false');
  }
}
detailRetry.addEventListener('click', loadDetail);
loadDetail();

const bookButton = document.querySelector('#book-button');
const bookingMessage = document.querySelector('#booking-message');
function renderBookingAction() {
  const button = document.querySelector('#book-button');
  const login = document.querySelector('#book-login');
  login.href = loginAddress();
  login.hidden = !Auth.state || !!Auth.state.user || currentListing.status !== 'available';
  const own = !!Auth.state?.user && currentListing.seller_user_id === Auth.state.user.id;
  document.querySelector('#own-listing-note').hidden = !own;
  button.hidden = !Auth.state?.user || currentListing.status !== 'available' || own;
  button.textContent = currentListing.deal_type === 'rent' ? 'Арендовать' : 'Купить';
  button.disabled = bookingPending;
}
bookButton.addEventListener('click', async () => {
  if (bookingPending) return;
  bookingPending = true;
  ++detailRequest; // Старое чтение не должно перекрыть результат нового действия.
  bookButton.disabled = true;
  bookingMessage.textContent = 'Создаём бронирование…';
  try {
    const result = await Auth.post(`/api/listings/${encodeURIComponent(id)}/book`);
    bookingMessage.textContent = result.message;
    currentListing.status = 'reserved';
    renderDetail(currentListing);
    document.querySelector('#booking-account').hidden = false;
  } catch (error) {
    bookingMessage.textContent = error.message;
    if (error.status === 401 || error.status === 403) {
      try { await Auth.load(); renderBookingAction(); } catch { /* Сообщение исходной ошибки остаётся. */ }
    }
  } finally {
    bookingPending = false;
    renderBookingAction();
    await loadDetail();
  }
});

window.addEventListener('focus', loadDetail);
