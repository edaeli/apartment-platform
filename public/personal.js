const viewsPage = location.pathname === '/view-history';
const personalBase = viewsPage ? '/view-history' : '/favorites';
const personalList = document.querySelector('#personal-list');
const personalMessage = document.querySelector('#personal-message');
const personalRetry = document.querySelector('#personal-retry');
const personalPagination = document.querySelector('#personal-pagination');
const personalTitle = viewsPage ? 'История просмотров' : 'Избранное';
document.title = personalTitle + ' — Свой адрес';
document.querySelector('#personal-title').textContent = personalTitle;
document.querySelector('#personal-note').textContent = viewsPage
  ? 'Только открытые вами подробные страницы. Повторное открытие учитывается не чаще раза в 30 минут.'
  : 'Сохранены именно эти объявления. Занятые и закрытые записи остаются здесь с прежними ссылками.';
let personalRequest;
function personalUrl(page) {
  const params = new URLSearchParams(location.search); params.set('page', page);
  return personalBase + '?' + params;
}
async function loadPersonal() {
  await authReady;
  if (!Auth.state) {
    personalMessage.textContent = 'Не удалось проверить вход. Обновите страницу после восстановления соединения.';
    personalList.setAttribute('aria-busy', 'false');
    return;
  }
  if (!Auth.state?.user) { location.replace(loginAddress()); return; }
  personalRequest?.abort(); const controller = new AbortController(); personalRequest = controller;
  const revision = favoriteRevision;
  personalList.replaceChildren(); personalList.setAttribute('aria-busy', 'true');
  personalPagination.hidden = personalRetry.hidden = true; personalMessage.textContent = 'Загружаем объявления…';
  try {
    const response = await fetch((viewsPage ? '/api/views' : '/api/favorites') + location.search, {cache:'no-store', signal:controller.signal});
    const data = await response.json();
    if (response.status === 401) { location.replace(loginAddress()); return; }
    if (!response.ok) throw new Error(data.error || 'Не удалось загрузить список.');
    if (controller.signal.aborted) return;
    // После удаления последней карточки на странице переходим на предыдущую непустую.
    if (data.total_pages && data.page > data.total_pages) {
      history.replaceState(null, '', personalUrl(data.total_pages)); loadPersonal(); return;
    }
    syncFavorites(data.items, revision);
    for (const item of data.items) {
      const card = listingCard(item);
      const note = document.createElement('p'); note.className = 'form-note';
      note.textContent = viewsPage
        ? `Последний учтённый просмотр: ${new Date(item.last_viewed_at * 1000).toLocaleString('ru-RU')} · Всего: ${item.view_count}`
        : `Добавлено: ${new Date(item.saved_at).toLocaleString('ru-RU')}`;
      card.querySelector('.card-content').append(note); personalList.append(card);
    }
    personalMessage.textContent = data.total ? `Объявлений: ${data.total}` : viewsPage
      ? 'Вы пока не открывали объявления после входа.' : 'В избранном пока пусто. Сохраните понравившееся жильё из каталога.';
    personalPagination.hidden = data.total_pages <= 1;
    const previous = document.querySelector('#personal-previous'), next = document.querySelector('#personal-next');
    previous.hidden = data.page <= 1; previous.href = personalUrl(data.page - 1);
    next.hidden = data.page >= data.total_pages; next.href = personalUrl(data.page + 1);
    document.querySelector('#personal-summary').textContent = `Страница ${data.page} из ${data.total_pages}`;
  } catch (error) {
    if (controller.signal.aborted) return;
    personalMessage.textContent = error.message; personalRetry.hidden = false;
  } finally { if (personalRequest === controller) personalList.setAttribute('aria-busy', 'false'); }
}
personalRetry.addEventListener('click', loadPersonal);
window.addEventListener('favorite-changed', loadPersonal);
loadPersonal();
