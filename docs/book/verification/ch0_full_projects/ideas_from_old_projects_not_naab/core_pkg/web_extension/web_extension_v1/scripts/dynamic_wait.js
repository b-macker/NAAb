// scripts/dynamic_wait.js
/**
 * Waits for an element matching selector to appear in DOM.
 * Returns a Promise that resolves with the element or rejects on timeout.
 */
function waitForSelector(selector, timeout = 5000) {
  return new Promise((resolve, reject) => {
    const interval = 200;
    const max = timeout / interval;
    let count = 0;
    const timer = setInterval(() => {
      const el = document.querySelector(selector);
      count++;
      if (el) {
        clearInterval(timer);
        resolve(el);
      } else if (count >= max) {
        clearInterval(timer);
        reject(new Error('Timeout waiting for ' + selector));
      }
    }, interval);
  });
}
// Expose to global
window.waitForSelector = waitForSelector;
