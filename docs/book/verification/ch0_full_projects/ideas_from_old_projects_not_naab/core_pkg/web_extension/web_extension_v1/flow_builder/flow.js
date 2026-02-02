// flow_builder/flow.js
/**
 * Executes a series of automation steps on the page.
 * Steps format: { selector: string, type: 'click'|'fill', value?: string, delay?: number }
 */
chrome.runtime.onMessage.addListener((msg) => {
  if (msg.action === 'runFlow' && Array.isArray(msg.steps)) {
    (async () => {
      for (const step of msg.steps) {
        try {
          const el = await waitForSelector(step.selector, step.timeout || 5000);
          if (step.type === 'click') {
            el.click();
          } else if (step.type === 'fill') {
            el.value = step.value || '';
            el.dispatchEvent(new Event('input', { bubbles: true }));
          }
        } catch (err) {
          console.error('[Flow] Step failed:', step, err);
        }
        // delay before next step
        await new Promise(res => setTimeout(res, step.delay != null ? step.delay : 300));
      }
    })();
  }
});

// utility to wait for element
async function waitForSelector(selector, timeout = 5000) {
  const pollInterval = 200;
  const maxAttempts = Math.ceil(timeout / pollInterval);
  for (let i = 0; i < maxAttempts; i++) {
    const el = document.querySelector(selector);
    if (el) return el;
    await new Promise(res => setTimeout(res, pollInterval));
  }
  throw new Error('Timeout waiting for selector: ' + selector);
}
