// scripts/api_mapper.js
/**
 * Wraps window.fetch to log API requests and responses.
 */
(function() {
  const originalFetch = window.fetch;
  window.fetch = async function(resource, init) {
    console.group('[Blank API] Request');
    console.log('Resource:', resource);
    console.log('Init:', init);
    console.groupEnd();
    const response = await originalFetch(resource, init);
    const clone = response.clone();
    clone.text().then(body => {
      console.group('[Blank API] Response');
      console.log('URL:', clone.url);
      console.log('Status:', clone.status);
      console.log('Body:', body);
      console.groupEnd();
    });
    return response;
  };
})();
