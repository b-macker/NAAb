// formula_builder/builder.js
/**
 * Injects Formula Builder UI into blankSidebar.
 * Allows applying custom JS expressions to a specified column.
 */
(function(){
  const SID = 'blankSidebar';
  function initFormulaBuilder() {
    const sb = document.getElementById(SID);
    if (!sb) return;
    if (document.getElementById('fb-panel')) return;
    const panel = document.createElement('div');
    panel.id = 'fb-panel';
    panel.innerHTML = `
      <h3>Formula Builder</h3>
      <div><label>Column Index: <input id="fb-col" type="number" min="0" value="0"/></label></div>
      <div><label>Expression: <input id="fb-expr" type="text" placeholder="e.g. parseFloat(val)*2"/></label></div>
      <button id="fb-apply">Apply</button>
    `;
    sb.querySelector('#sections').appendChild(panel);

    document.getElementById('fb-apply').addEventListener('click', () => {
      const col = parseInt(document.getElementById('fb-col').value, 10);
      const expr = document.getElementById('fb-expr').value;
      document.querySelectorAll('table').forEach(tbl => {
        Array.from(tbl.rows).slice(1).forEach(row => {
          const cell = row.cells[col];
          if (cell) {
            try {
              const val = cell.innerText;
              const res = new Function('val', 'return ' + expr)(val);
              cell.innerText = res;
            } catch (err) {
              console.error('[FormulaBuilder] Error:', err);
            }
          }
        });
      });
    });
  }

  chrome.runtime.onMessage.addListener(msg => {
    if (msg.action === 'toggleSidebar') {
      initFormulaBuilder();
    }
  });
})();
