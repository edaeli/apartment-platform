const bookingsList = document.querySelector('#bookings-list');
const bookingsMessage = document.querySelector('#bookings-message');
const bookingsRetry = document.querySelector('#bookings-retry');
async function loadBookings() {
  bookingsList.replaceChildren();
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
    for (const booking of data.items) {
      const item = booking.listing;
      const card = document.createElement('article');
      card.className = 'booking-card';
      const gallery = document.createElement('div');
      gallery.className = 'booking-photos';
      for (const photo of item.photos.length ? item.photos : [null]) {
        const image = document.createElement('img');
        image.loading = 'lazy';
        showPhoto(image, photo);
        gallery.append(image);
      }
      const info = document.createElement('div');
      const title = document.createElement('h2');
      const link = document.createElement('a');
      link.className = 'inline-link';
      link.href = `/listings/${item.id}`;
      link.textContent = listingTitle(item);
      title.append(link);
      info.append(title);
      for (const text of [
        `${item.address} · ${item.district}`,
        `${booking.deal_type === 'rent' ? 'Аренда' : 'Покупка'} · ${item.rooms} комн. · ${item.area} м²`,
        'Состояние бронирования: ' + ({ active: 'Активно', released: 'Освобождено', resold: 'Выставлено на перепродажу' }[booking.status] || 'Неизвестно'),
        `Создано: ${new Date(booking.created_at).toLocaleString('ru-RU')} · № ${booking.id}`
      ]) {
        const p = document.createElement('p'); p.textContent = text; info.append(p);
      }
      const price = document.createElement('p');
      price.className = 'price';
      showPrice(price, { price: booking.price_at_booking, deal_type: booking.deal_type });
      info.append(price);
      card.append(gallery, info);
      bookingsList.append(card);
    }
    bookingsMessage.hidden = data.items.length > 0;
    bookingsMessage.textContent = 'У вас пока нет бронирований. Выберите жильё в каталоге.';
  } catch (error) {
    bookingsMessage.textContent = error.message || 'Не удалось связаться с сервером.';
    bookingsRetry.hidden = false;
  } finally { bookingsList.setAttribute('aria-busy', 'false'); }
}
bookingsRetry.addEventListener('click', loadBookings);
// Завершаем общий запрос шапки, чтобы параллельные гостевые запросы не меняли cookie.
authReady.then(loadBookings);
