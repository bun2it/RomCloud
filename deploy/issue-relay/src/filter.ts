/**
 * Sensitive Data Filter
 *
 * Removes potentially sensitive information from bug reports.
 * Applied both client-side (in app) and server-side (in worker).
 */

// Patterns to filter
const SENSITIVE_PATTERNS = [
  // Token patterns
  { pattern: /ghp_[a-zA-Z0-9]{36}/g, replacement: '[GITHUB_TOKEN]' },
  { pattern: /github_pat_[a-zA-Z0-9_]{80,}/g, replacement: '[GITHUB_TOKEN]' },
  { pattern: /xox[baprs]-[a-zA-Z0-9]{10,}/g, replacement: '[SLACK_TOKEN]' },
  { pattern: /sk-[a-zA-Z0-9]{48}/g, replacement: '[OPENAI_KEY]' },

  // Password patterns
  { pattern: /password["\s:=]+[^\s,}]+/gi, replacement: 'password=[REDACTED]' },
  { pattern: /passwd["\s:=]+[^\s,}]+/gi, replacement: 'passwd=[REDACTED]' },
  { pattern: /secret["\s:=]+[^\s,}]+/gi, replacement: 'secret=[REDACTED]' },
  { pattern: /api_key["\s:=]+[^\s,}]+/gi, replacement: 'api_key=[REDACTED]' },
  { pattern: /apikey["\s:=]+[^\s,}]+/gi, replacement: 'apikey=[REDACTED]' },

  // IP addresses (internal ranges)
  { pattern: /\b(10\.\d{1,3}\.\d{1,3}\.\d{1,3})/g, replacement: '[PRIVATE_IP]' },
  { pattern: /\b(172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})/g, replacement: '[PRIVATE_IP]' },
  { pattern: /\b(192\.168\.\d{1,3}\.\d{1,3})/g, replacement: '[PRIVATE_IP]' },
  { pattern: /\b(127\.\d{1,3}\.\d{1,3}\.\d{1,3})/g, replacement: '[LOCALHOST]' },

  // MAC addresses
  { pattern: /([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}/g, replacement: '[MAC_ADDRESS]' },

  // Serial numbers (generic pattern - not specific to device)
  { pattern: /\b[A-Z0-9]{8,}-[A-Z0-9]{4,}-[A-Z0-9]{4,}-[A-Z0-9]{4,}-[A-Z0-9]{12,}\b/g, replacement: '[SERIAL]' },
  { pattern: /\b[A-Fa-f0-9]{32}\b/g, replacement: '[HEX_ID]' },

  // Email addresses
  { pattern: /[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}/g, replacement: '[EMAIL]' },

  // File paths that might be user-specific
  { pattern: /\/home\/[a-zA-Z0-9_]+\//g, replacement: '/home/[user]/' },
  { pattern: /\/Users\/[a-zA-Z0-9_.]+\//g, replacement: '/Users/[user]/' },
  { pattern: /C:\\Users\\[a-zA-Z0-9_.]+\\/gi, replacement: 'C:\\Users\\[user]\\' },

  // Android/iOS device identifiers
  { pattern: /\b[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}\b/gi, replacement: '[UUID]' },

  // Raw hardware IDs (our own output is OK, but raw chip IDs are not)
  { pattern: /\b[a-fA-F0-9]{16,}\b/g, replacement: '[HW_ID]' },
];

// Paths to redact
const SENSITIVE_PATHS = [
  '/api/',
  '/auth/',
  '/login',
  '/token',
  '/secret',
  '/private/',
  '/.ssh/',
  '/.config/',
  '/credentials',
];

export function filterSensitiveData(text: string): string {
  if (!text || typeof text !== 'string') {
    return '';
  }

  let filtered = text;

  // Apply all patterns
  for (const { pattern, replacement } of SENSITIVE_PATTERNS) {
    filtered = filtered.replace(pattern, replacement);
  }

  // Redact sensitive path mentions
  for (const path of SENSITIVE_PATHS) {
    const pathRegex = new RegExp(path.replace(/\//g, '\\/'), 'gi');
    filtered = filtered.replace(pathRegex, `[REDACTED_PATH]`);
  }

  // Remove very long random-looking strings (likely tokens or keys)
  filtered = filtered.replace(/[A-Za-z0-9+/=]{64,}/g, (match) => {
    // Don't redact if it looks like a valid base64 string with reasonable length
    if (match.length >= 64 && match.length <= 128 && /^[A-Za-z0-9+/=]+$/.test(match)) {
      return '[TOKEN]';
    }
    return match;
  });

  // Normalize multiple spaces and newlines
  filtered = filtered.replace(/\s+/g, ' ').trim();

  return filtered;
}

// Check if text contains potential sensitive data
export function containsSensitiveData(text: string): boolean {
  if (!text) return false;

  // Quick checks
  const lower = text.toLowerCase();
  if (lower.includes('password') ||
      lower.includes('token') ||
      lower.includes('secret') ||
      lower.includes('api_key') ||
      lower.includes('ghp_') ||
      lower.includes('github_pat_')) {
    return true;
  }

  return false;
}
