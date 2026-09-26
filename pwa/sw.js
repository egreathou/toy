// ToY service worker — minimal stub.
// A fetch handler is required for installability; caching and push
// notifications are planned but not yet implemented (see DESIGN.md).

self.addEventListener('install', () => {
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', (event) => {
  event.respondWith(fetch(event.request));
});