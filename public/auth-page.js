const registration = location.pathname === '/register';
const form = document.querySelector('#auth-form');
const submit = document.querySelector('#auth-submit');
const message = document.querySelector('#auth-message');
// Возврат — только на известную страницу нашего сайта, без внешнего перенаправления.
let next = '/';
try {
  const target = new URL(new URLSearchParams(location.search).get('next') || '/', location.origin);
  if (target.origin === location.origin &&
      (['/', '/my-bookings', '/favorites', '/view-history'].includes(target.pathname) || /^\/listings\/\d+$/.test(target.pathname))) {
    next = target.pathname + target.search + target.hash;
  }
} catch { /* Остаёмся в каталоге. */ }
document.querySelector('#auth-back').href = next;
const title = registration ? 'Создать аккаунт' : 'Войти';
document.querySelector('#auth-title').textContent = submit.textContent = title;
document.title = `${title} — Свой адрес`;
document.querySelector('#name-label').hidden = !registration;
form.elements.name.required = registration;
form.elements.password.autocomplete = registration ? 'new-password' : 'current-password';
const switchLink = document.querySelector('#auth-switch');
switchLink.textContent = registration ? 'Уже есть аккаунт? Войти' : 'Создать аккаунт';
switchLink.href = (registration ? '/login' : '/register') + '?next=' + encodeURIComponent(next);
authReady.then(state => {
  submit.disabled = !state;
  message.textContent = state ? (state.user ? `Вы уже вошли как ${state.user.name}. Можно вернуться на сайт или войти в другой аккаунт.` : '')
    : 'Не удалось подготовить форму. Обновите страницу.';
});
form.addEventListener('submit', async event => {
  event.preventDefault();
  if (submit.disabled) return;
  submit.disabled = true;
  message.textContent = registration ? 'Создаём аккаунт…' : 'Проверяем данные…';
  const data = { email: form.elements.email.value, password: form.elements.password.value };
  if (registration) data.name = form.elements.name.value;
  try {
    await Auth.post(registration ? '/api/auth/register' : '/api/auth/login', data);
    form.elements.password.value = '';
    location.replace(next); // Бронирование автоматически НЕ выполняется.
  } catch (error) {
    message.textContent = error.message;
    if (error.status === 403) {
      try { await Auth.load(); } catch { message.textContent += ' Не удалось обновить сессию.'; }
    }
    submit.disabled = false;
  }
});
