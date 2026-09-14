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
  const becameReserved = currentListing?.status === 'available' && item.status === 'reserved';
  currentListing = item;
  // Возвращаем тот же узел результата рядом с действием после успешного повторного GET.
  const action = document.querySelector('#booking-action');
  const feedback = document.querySelector('#booking-feedback');
  if (feedback.parentElement !== action) action.append(feedback);
  renderBookingAction();
  // Собственный POST имеет приоритет. Запоздалые GET также отсекаются detailRequest.
  if (becameReserved && !bookingPending &&
      !['success', 'error'].includes(feedback.dataset.outcome)) {
    showBookingFeedback('Это жильё только что забронировали. Выберите другое объявление', 'warning');
  }
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
  renderModelEstimate(item);
  const status = document.querySelector('#detail-status');
  status.textContent = { available: 'Доступно', reserved: 'Забронировано', closed: 'Закрыто' }[item.status] || item.status;
  status.dataset.status = item.status;
  const characteristics = document.querySelector('#characteristics');
  characteristics.replaceChildren();
  for (const [name, value] of [
    ['Объявление', `№ ${item.id}`],
    ...(item.renovation ? [['Ремонт', renovationLabel(item.renovation)]] : []), ['Тип объекта', item.kind === 'house' ? 'Дом' : 'Квартира'],
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
  // Фоновое чтение не прячет уже открытую страницу и не сдвигает кнопку/сообщение.
  detail.hidden = !currentListing;
  detailRetry.hidden = true;
  detailMessage.hidden = false;
  detailMessage.textContent = 'Загружаем объявление…';
  detail.setAttribute('aria-busy', 'true');
  try {
    const response = await fetch(`/api/listings/${encodeURIComponent(id)}`, { cache: 'no-store' });
    const item = await response.json();
    if (request !== detailRequest) return;
    if (!response.ok) {
      detailMessage.textContent = response.status === 404
        ? 'Объявление не найдено. Проверьте адрес или вернитесь в каталог.'
        : item.error || 'Не удалось загрузить объявление.';
      if (response.status === 404) {
        // Исчезновение объявления не должно спрятать уже полученный результат операции.
        if (!bookingFeedback.hidden) detail.before(bookingFeedback);
        detail.hidden = true;
      }
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
const bookingFeedback = document.querySelector('#booking-feedback');
function showBookingFeedback(message, outcome) {
  bookingFeedback.hidden = false;
  bookingFeedback.dataset.outcome = outcome;
  bookingFeedback.setAttribute('role', outcome === 'error' ? 'alert' : 'status');
  bookingFeedback.setAttribute('aria-live', outcome === 'error' ? 'assertive' : 'polite');
  bookingMessage.textContent = message;
  document.querySelector('#booking-icon').textContent = { warning: '⚠', error: '⚠', success: '✓', info: 'ⓘ', pending: '…' }[outcome];
  document.querySelector('#booking-account').hidden = outcome !== 'success';
  if (outcome !== 'pending' && outcome !== 'info') revealBookingFeedback();
}
function revealBookingFeedback() {
  // Результат рядом с действием; после обновления данных возвращаем его в поле зрения.
  bookingFeedback.focus({ preventScroll: true });
  bookingFeedback.scrollIntoView({ block: 'center', behavior: 'instant' });
}
function renderBookingAction() {
  const button = document.querySelector('#book-button');
  const login = document.querySelector('#book-login');
  const unavailable = currentListing.status !== 'available';
  // Нейтральное пояснение — только когда результата действия ещё нет.
  if (unavailable && bookingFeedback.hidden && !bookingPending) {
    showBookingFeedback('Объявление недоступно для нового бронирования', 'info');
  } else if (!unavailable && bookingFeedback.dataset.outcome === 'info') {
    bookingFeedback.hidden = true;
  }
  document.querySelector('#booking-controls').hidden = unavailable;
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
  // Фоновый GET при фокусе окна мог опередить уже начатое нажатие.
  // Молчаливый return здесь оставлял пользователя без результата и без POST/409.
  if (currentListing?.status !== 'available') {
    showBookingFeedback('Не удалось забронировать: это жильё уже занято. Выберите другое объявление', 'error');
    return;
  }
  bookingPending = true;
  ++detailRequest; // Старое чтение не должно перекрыть результат нового действия.
  bookButton.disabled = true;
  showBookingFeedback('Создаём бронирование…', 'pending');
  try {
    const result = await Auth.post(`/api/listings/${encodeURIComponent(id)}/book`);
    if (!Number.isSafeInteger(result.id) || result.id <= 0 || result.listing_id !== Number(id) || result.status !== 'active') {
      throw new Error('Некорректное подтверждение бронирования');
    }
    showBookingFeedback(result.message || 'Бронирование создано. Оплата не производится', 'success');
    currentListing.status = 'reserved';
    renderDetail(currentListing);
  } catch (error) {
    if (error.status === 409) {
      // Конфликт уже подтверждён сервером: повторное чтение может быть недоступно.
      currentListing.status = 'reserved';
      renderDetail(currentListing);
      showBookingFeedback('Не удалось забронировать: это жильё уже занято. Выберите другое объявление', 'error');
    } else {
      showBookingFeedback(error.status ? error.message
        : 'Не удалось связаться с сервером или прочитать ответ. Бронирование не подтверждено. Проверьте «Мои бронирования» перед повтором.', 'error');
    }
    if (error.status === 401 || error.status === 403) {
      try { await Auth.load(); renderBookingAction(); } catch { /* Сообщение исходной ошибки остаётся. */ }
    }
  } finally {
    bookingPending = false;
    renderBookingAction();
    await loadDetail();
    revealBookingFeedback();
  }
});

window.addEventListener('focus', loadDetail);

function renderModelEstimate(item) {
  const estimate = item.price_estimate;
  const amount = estimate?.amount_amd;
  const valid = estimate?.status === 'available' && estimate.reason !== 'lower_clipped' && typeof amount === 'number' &&
    Number.isFinite(amount) && amount >= 1 && amount <= Number.MAX_SAFE_INTEGER;
  document.querySelector('#model-price').textContent = valid
    ? `Учебная оценка модели: ${numberFormat.format(Math.round(amount))} ֏ ${item.deal_type === 'rent' ? 'в месяц' : 'за объект'}`
    : estimate?.reason === 'lower_clipped'
      ? 'Учебная оценка для этого объекта недоступна'
      : 'Учебная оценка модели недоступна';
}
