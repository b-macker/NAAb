// formula_builder/types.js
/**
 * Detects data type of a string value.
 * Returns 'date', 'number', or 'text'.
 */
function detectType(val) {
  if (!isNaN(Date.parse(val))) return 'date';
  if (!isNaN(parseFloat(val))) return 'number';
  return 'text';
}
