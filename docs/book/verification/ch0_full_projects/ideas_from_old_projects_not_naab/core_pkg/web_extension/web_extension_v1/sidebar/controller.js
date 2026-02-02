(function() {
  const ID = 'blankSidebar';
  function toggle() {
    let sb = document.getElementById(ID);
    if (sb) {
      sb.style.display = sb.style.display === 'none' ? '' : 'none';
      return;
    }
    sb = document.createElement('div');
    sb.id = ID;
    Object.assign(sb.style, {
      position: 'fixed', top: '0', right: '0',
      width: '300px', height: '100%', background: '#222',
      color: '#fff', padding: '10px', zIndex: 2147483647, overflowY: 'auto'
    });
    sb.innerHTML = `
      <div style="display:flex;justify-content:space-between;align-items:center;">
        <h2 style="margin:0;">blank</h2>
        <button id="closeBtn" style="background:none;border:none;color:#fff;font-size:20px;cursor:pointer;">✕</button>
      </div>
      <hr style="border-color:#555"/>
      <div id="sections"></div>
    `;
    document.body.appendChild(sb);
    sb.querySelector('#closeBtn').onclick = toggle;
  }
  chrome.runtime.onMessage.addListener(msg => {
    if (msg.action === 'toggleSidebar') toggle();
  });
  window.addEventListener('load', toggle);
})();