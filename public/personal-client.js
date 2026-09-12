function listingCard(item, returnTo = '/#catalog') {
  const card = document.querySelector('#listing-template').content.cloneNode(true);
  showPhoto(card.querySelector('img'), item.photos[0]);
  const href = `/listings/${item.id}?return_to=${encodeURIComponent(returnTo)}`;
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
  const status = card.querySelector('.availability');
  status.textContent = {available: 'Доступно', reserved: 'Забронировано', closed: 'Закрыто'}[item.status] || item.status;
  status.dataset.status = item.status;
  card.querySelector('.card-content').append(favoriteControl(item));
  if (item.reason) {
    const reason = document.createElement('p'); reason.className = 'recommendation-reason';
    reason.textContent = item.reason; card.querySelector('.card-content').append(reason);
  }
  return card;
}

// Локальное состояние для согласования двух карточек одного объявления на странице.
// На сервер отправляется явное PUT или DELETE, никогда не toggle.
const favoriteStates = new Map();
const favoritePending = new Set();
function updateFavoriteButtons(id) {
  document.querySelectorAll(`[data-favorite-id="${id}"]`).forEach(button => {
    const saved = favoriteStates.get(id);
    button.textContent = saved ? '♥ Убрать из избранного' : '♡ В избранное';
    button.setAttribute('aria-pressed', String(saved));
    button.disabled = favoritePending.has(id);
  });
}
function favoriteControl(item) {
  const container = document.createElement('div'); container.className = 'favorite-control';
  if (!Auth.state?.user) {
    const link = document.createElement('a'); link.className = 'inline-link';
    link.textContent = '♡ Войти, чтобы сохранить'; link.href = loginAddress(); container.append(link);
    return container;
  }
  if (!favoriteStates.has(item.id)) favoriteStates.set(item.id, item.is_favorite);
  const button = document.createElement('button'); button.type = 'button'; button.className = 'favorite-button';
  button.dataset.favoriteId = item.id;
  const saved = favoriteStates.get(item.id);
  button.textContent = saved ? '♥ Убрать из избранного' : '♡ В избранное';
  button.setAttribute('aria-pressed', String(saved)); button.disabled = favoritePending.has(item.id);
  const message = document.createElement('p'); message.className = 'form-message'; message.setAttribute('role', 'status');
  button.addEventListener('click', async () => {
    if (favoritePending.has(item.id)) return;
    const desired = !favoriteStates.get(item.id);
    favoritePending.add(item.id); updateFavoriteButtons(item.id); message.textContent = '';
    try {
      const result = await Auth.mutate(`/api/favorites/${item.id}`, desired ? 'PUT' : 'DELETE');
      favoriteStates.set(item.id, result.is_favorite);
      window.dispatchEvent(new CustomEvent('favorite-changed', { detail: { id: item.id, saved: result.is_favorite } }));
    } catch (error) {
      message.textContent = error.message;
      if (error.status === 401) {
        const link = document.createElement('a'); link.className = 'inline-link';
        link.href = loginAddress(); link.textContent = ' Войти'; message.append(link);
      }
    } finally { favoritePending.delete(item.id); updateFavoriteButtons(item.id); }
  });
  container.append(button, message);
  return container;
}
