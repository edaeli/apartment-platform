// Секретная cookie недоступна JavaScript. Здесь только профиль и CSRF-токен формы.
const Auth = {
  state: null,
  async load() {
    const response = await fetch('/api/auth/me', { cache: 'no-store' });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Не удалось проверить вход.');
    this.state = result;
    renderAccount(result.user);
    return result;
  },
  async post(path, data = {}) { return this.mutate(path, 'POST', data); },
  async mutate(path, method, data = {}) {
    if (!this.state) await this.load();
    const response = await fetch(path, {
      method,
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': this.state.csrf_token },
      body: JSON.stringify(data)
    });
    const result = await response.json();
    if (!response.ok) {
      const error = new Error(result.error || 'Не удалось выполнить действие.');
      error.status = response.status;
      throw error;
    }
    if (result.csrf_token) {
      this.state = result;
      renderAccount(result.user);
    }
    return result;
  }
};

function loginAddress() {
  const target = ['/login', '/register'].includes(location.pathname)
    ? new URLSearchParams(location.search).get('next') || '/'
    : location.pathname + location.search;
  return '/login?next=' + encodeURIComponent(target);
}

function renderAccount(user) {
  const nav = document.querySelector('#account-nav');
  if (!nav) return;
  nav.replaceChildren();
  const link = (label, href) => {
    const node = document.createElement('a');
    node.textContent = label;
    node.href = href;
    nav.append(node);
  };
  if (user) {
    const name = document.createElement('span');
    name.className = 'account-name';
    name.textContent = user.name;
    name.title = user.email;
    nav.append(name);
    link('Мои бронирования', '/my-bookings');
    link('Избранное', '/favorites');
    link('История просмотров', '/view-history');
    const logout = document.createElement('button');
    logout.type = 'button';
    logout.className = 'text-button';
    logout.textContent = 'Выйти';
    logout.addEventListener('click', async () => {
      logout.disabled = true;
      try {
        await Auth.post('/api/auth/logout');
        Auth.state = null;
        location.replace(['/my-bookings', '/bookings.html', '/favorites', '/view-history', '/personal.html'].includes(location.pathname) ? '/' : location.href);
      } catch (error) {
        logout.disabled = false;
        document.querySelector('#account-message').textContent = error.message;
      }
    });
    nav.append(logout);
  } else {
    link('Войти', loginAddress());
    link('Регистрация', loginAddress().replace('/login?', '/register?'));
  }
}

const authReady = Auth.load().catch(error => {
  document.querySelector('#account-nav').textContent = 'Вход временно недоступен';
  document.querySelector('#account-message').textContent = error.message + ' Обновите страницу.';
  return null;
});
// Возврат из памяти браузера не должен показывать старый личный кабинет после выхода.
window.addEventListener('pageshow', event => { if (event.persisted) location.reload(); });
