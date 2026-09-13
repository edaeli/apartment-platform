// Общие функции двух страниц: текст из базы вставляется только через textContent.
const numberFormat = new Intl.NumberFormat('ru-RU');
function listingTitle(item) {
  return item.kind === 'house' ? `Дом · ${numberFormat.format(item.area)} м²`
    : `${item.rooms}-комнатная квартира · ${numberFormat.format(item.area)} м²`;
}
function showPrice(element, item) {
  element.textContent = `${numberFormat.format(item.price)} ֏`;
  const period = document.createElement('span');
  period.textContent = item.deal_type === 'rent' ? ' в месяц' : ' за объект';
  element.append(period);
}
function showPhoto(image, photo) {
  image.onerror = () => {
    image.onerror = null;
    image.src = '/images/placeholder.svg';
    image.alt = 'Фотография недоступна';
  };
  image.src = photo?.url || '/images/placeholder.svg';
  image.alt = photo ? `${photo.caption} — демонстрационная фотография` : 'Фотография отсутствует';
}

function renovationLabel(value) {
  return {unspecified: 'Не указано', needs_repair: 'Требует ремонта', cosmetic: 'Косметический',
    good: 'Хороший', designer: 'Дизайнерский'}[value] || 'Не указано';
}
