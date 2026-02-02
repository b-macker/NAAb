// scripts/xpath_tracker.js
/**
 * Logs the XPath of clicked elements to the console.
 */
document.addEventListener('click', event => {
  const getXPath = (el) => {
    if (el.id) return 'id("' + el.id + '")';
    if (el === document.body) return '/html/body';
    let ix = 0;
    const siblings = el.parentNode ? el.parentNode.children : [];
    for (let i = 0; i < siblings.length; i++) {
      const sib = siblings[i];
      if (sib === el) {
        return getXPath(el.parentNode) + '/' + el.tagName.toLowerCase() + '[' + (ix + 1) + ']';
      }
      if (sib.nodeType === 1 && sib.tagName === el.tagName) ix++;
    }
  };
  console.log('[XPath]', getXPath(event.target));
});
