const recommendationsList = document.querySelector('#recommendations-list');
const recommendationsMessage = document.querySelector('#recommendations-message');
const recommendationsRetry = document.querySelector('#recommendations-retry');
let recommendationsRequest;
async function loadRecommendations() {
  await authReady;
  recommendationsRequest?.abort();
  const controller = new AbortController(); recommendationsRequest = controller;
  recommendationsList.setAttribute('aria-busy', 'true'); recommendationsRetry.hidden = true;
  recommendationsMessage.textContent = 'Загружаем подборку…';
  try {
    const response = await fetch('/api/recommendations', {cache: 'no-store', signal: controller.signal});
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'Не удалось загрузить подборку.');
    if (controller.signal.aborted) return;
    recommendationsList.replaceChildren(...data.items.map(item => listingCard(item)));
    document.querySelector('#recommendations-note').textContent = data.personalized
      ? 'По районам и ценам вашего избранного и просмотров. Аренда и продажа учитываются отдельно.'
      : 'Обычная подборка доступного жилья из каталога.';
    recommendationsMessage.textContent = data.count ? '' : 'Пока нет доступных вариантов для подборки.';
  } catch (error) {
    if (controller.signal.aborted) return;
    recommendationsList.replaceChildren(); recommendationsMessage.textContent = error.message;
    recommendationsRetry.hidden = false;
  } finally {
    if (recommendationsRequest === controller) recommendationsList.setAttribute('aria-busy', 'false');
  }
}
recommendationsRetry.addEventListener('click', loadRecommendations);
window.addEventListener('favorite-changed', loadRecommendations);
loadRecommendations();
