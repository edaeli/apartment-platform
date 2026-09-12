const bookingsList = document.querySelector('#bookings-list');
const bookingsMessage = document.querySelector('#bookings-message');
const bookingsRetry = document.querySelector('#bookings-retry');
const feedback = document.querySelector('#booking-feedback');
let actionPending = false;

function paragraph(parent, text, className = '') {
  const p = document.createElement('p'); p.textContent = text; p.className = className; parent.append(p);
  return p;
}
function button(parent, text, className = 'action-button') {
  const b = document.createElement('button'); b.type = 'button'; b.textContent = text; b.className = className; parent.append(b);
  return b;
}
function listingLink(parent, id, text) {
  const link = document.createElement('a'); link.href = `/listings/${id}`;
  link.className = 'inline-link'; link.textContent = text; parent.append(link);
}

async function performAction(booking, resale, price) {
  if (actionPending) return;
  actionPending = true;
  bookingsList.querySelectorAll('button, input').forEach(node => { node.disabled = true; });
  feedback.textContent = resale ? 'Создаём новое объявление…' : 'Освобождаем жильё…';
  try {
    const result = await Auth.post(`/api/bookings/${booking.id}/${resale ? 'resell' : 'release'}`, resale ? { price } : {});
    feedback.textContent = result.message + ' ';
    if (resale) listingLink(feedback, result.listing_id, 'Открыть новое объявление →');
    else listingLink(feedback, booking.listing_id, 'Открыть освобождённое объявление →');
    await loadBookings();
  } catch (error) {
    feedback.textContent = error.message || 'Не удалось выполнить действие.';
    // Читаем свежую историю, но никогда не повторяем изменяющий запрос автоматически.
    if ([401, 403, 409].includes(error.status)) await loadBookings();
  } finally {
    actionPending = false;
    bookingsList.querySelectorAll('button, input').forEach(node => { node.disabled = false; });
  }
}

function addActions(info, booking) {
  const actions = document.createElement('div'); actions.className = 'booking-controls'; info.append(actions);
  const resale = booking.deal_type === 'sale';
  const open = button(actions, resale ? 'Выставить на продажу' : 'Освободить');
  const panel = document.createElement('form'); panel.className = 'booking-form'; panel.hidden = true; actions.append(panel);
  let price;
  if (resale) {
    const label = document.createElement('label'); label.textContent = 'Новая цена, ֏';
    price = document.createElement('input'); price.type = 'text'; price.inputMode = 'numeric';
    price.name = 'price'; price.required = true; price.pattern = '[0-9]+'; price.maxLength = 13;
    price.placeholder = 'Например, 55000000'; label.append(price); panel.append(label);
    paragraph(panel, 'От 1 до 1 000 000 000 000 драмов, только целые числа. Будет создано новое объявление без оплаты.', 'form-note');
  } else {
    paragraph(panel, 'Освободить это жильё? Бронирование останется в истории, а объявление станет доступно другим пользователям.');
  }
  const confirm = button(panel, resale ? 'Опубликовать объявление' : 'Да, освободить'); confirm.type = 'submit';
  const cancel = button(panel, 'Отмена', 'reset-button');
  open.addEventListener('click', () => { panel.hidden = false; open.hidden = true; (price || confirm).focus(); });
  cancel.addEventListener('click', () => { panel.hidden = true; open.hidden = false; open.focus(); });
  panel.addEventListener('submit', event => {
    event.preventDefault();
    if (resale && (!/^[0-9]+$/.test(price.value) || Number(price.value) < 1 || Number(price.value) > 1000000000000)) {
      feedback.textContent = 'Введите целую цену от 1 до 1 000 000 000 000 драмов.'; price.focus(); return;
    }
    performAction(booking, resale, price?.value); // Передаём исходную строку, сервер проверяет её независимо.
  });
}

function bookingCard(booking) {
  const item = booking.listing;
  const card = document.createElement('article'); card.className = 'booking-card';
  const gallery = document.createElement('div'); gallery.className = 'booking-photos';
  for (const photo of item.photos.length ? item.photos : [null]) {
    const image = document.createElement('img'); image.loading = 'lazy'; showPhoto(image, photo); gallery.append(image);
  }
  const info = document.createElement('div');
  const title = document.createElement('h3'); listingLink(title, item.id, listingTitle(item)); info.append(title);
  const state = { active: 'Активно', released: 'Освобождено', resold: 'Выставлено на перепродажу' }[booking.status] || 'Неизвестно';
  paragraph(info, `${item.address} · ${item.district}`);
  paragraph(info, `${booking.deal_type === 'rent' ? 'Аренда' : 'Покупка'} · ${item.rooms} комн. · ${numberFormat.format(item.area)} м²`);
  paragraph(info, `Состояние бронирования: ${state}`);
  paragraph(info, `Создано: ${new Date(booking.created_at).toLocaleString('ru-RU')} · № ${booking.id}`);
  if (booking.ended_at) paragraph(info, `Завершено: ${new Date(booking.ended_at).toLocaleString('ru-RU')}`);
  paragraph(info, 'Цена на момент бронирования', 'form-note');
  const price = paragraph(info, '', 'price');
  showPrice(price, { price: booking.price_at_booking, deal_type: booking.deal_type });
  if (booking.resale_listing_id) {
    const p = paragraph(info, ''); listingLink(p, booking.resale_listing_id, `Новое объявление № ${booking.resale_listing_id} →`);
  }
  if (booking.status === 'active') addActions(info, booking);
  card.append(gallery, info);
  return card;
}

async function loadBookings() {
  bookingsList.hidden = true;
  bookingsList.setAttribute('aria-busy', 'true');
  bookingsRetry.hidden = true;
  bookingsMessage.hidden = false;
  bookingsMessage.textContent = 'Загружаем ваши бронирования…';
  try {
    const state = await Auth.load();
    if (!state.user) { location.replace('/login?next=%2Fmy-bookings'); return; }
    const response = await fetch('/api/bookings', { cache: 'no-store' });
    const data = await response.json();
    if (response.status === 401) { location.replace('/login?next=%2Fmy-bookings'); return; }
    if (!response.ok) throw new Error(data.error || 'Не удалось загрузить бронирования.');
    const active = document.querySelector('#active-bookings');
    const history = document.querySelector('#history-bookings');
    active.replaceChildren(); history.replaceChildren();
    for (const booking of data.items) (booking.status === 'active' ? active : history).append(bookingCard(booking));
    document.querySelector('#active-empty').hidden = active.children.length > 0;
    document.querySelector('#history-empty').hidden = history.children.length > 0;
    bookingsList.querySelectorAll('button, input').forEach(node => { node.disabled = actionPending; });
    bookingsList.hidden = false;
    bookingsMessage.hidden = true;
  } catch (error) {
    bookingsMessage.textContent = error.message || 'Не удалось связаться с сервером.';
    bookingsRetry.hidden = false;
  } finally { bookingsList.setAttribute('aria-busy', 'false'); }
}
bookingsRetry.addEventListener('click', loadBookings);
authReady.then(loadBookings);
