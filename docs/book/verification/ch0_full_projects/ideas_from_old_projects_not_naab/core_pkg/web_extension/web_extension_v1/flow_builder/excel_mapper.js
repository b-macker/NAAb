// flow_builder/excel_mapper.js
/**
 * Parses a CSV file where each row defines a step:
 * selector,type,value?,delay?
 * Example row: "button.next,click,,500"
 * Calls callback with array of step objects.
 */
function parseCSVFile(file, callback) {
  const reader = new FileReader();
  reader.onload = (e) => {
    const lines = e.target.result.trim().split(/\r?\n/);
    const steps = lines.map(line => {
      const [selector, type, value, delay] = line.split(',');
      return {
        selector: selector.trim(),
        type: type.trim(),
        value: value != null ? value.trim() : undefined,
        delay: delay ? parseInt(delay, 10) : undefined
      };
    });
    callback(steps);
  };
  reader.readAsText(file);
}
