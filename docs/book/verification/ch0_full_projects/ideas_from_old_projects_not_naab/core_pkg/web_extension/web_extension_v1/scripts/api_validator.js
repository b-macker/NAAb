// scripts/api_validator.js
/**
 * Validates that required keys exist in params object.
 * Throws error if missing.
 */
export function validateParams(params, requiredKeys) {
  requiredKeys.forEach(key => {
    if (!(key in params)) {
      throw new Error(`[Blank API Validator] Missing parameter: ${key}`);
    }
  });
  return true;
}
