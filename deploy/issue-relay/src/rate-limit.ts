/**
 * Rate Limiting Module
 *
 * Simple IP-based rate limiting using KV store.
 * Default: 10 requests per hour per IP.
 */

interface RateLimitResult {
  allowed: boolean;
  remaining: number;
  retryAfter?: number; // seconds until reset
}

// Rate limit configuration
const RATE_LIMIT_WINDOW = 60 * 60; // 1 hour in seconds
const RATE_LIMIT_MAX = 10; // requests per window

interface RateLimitEntry {
  count: number;
  windowStart: number;
}

// Get rate limit key for an IP
function getRateLimitKey(ip: string): string {
  return `ratelimit:${ip}`;
}

// Check if request is allowed under rate limit
export async function checkRateLimit(
  kv: KVNamespace,
  clientIP: string
): Promise<RateLimitResult> {
  // Skip rate limiting for health checks from known IPs
  if (clientIP === 'localhost' || clientIP === '127.0.0.1') {
    return { allowed: true, remaining: RATE_LIMIT_MAX };
  }

  try {
    const key = getRateLimitKey(clientIP);
    const data = await kv.get(key, 'json') as RateLimitEntry | null;
    const now = Math.floor(Date.now() / 1000);

    if (!data) {
      // No record exists - allow
      return { allowed: true, remaining: RATE_LIMIT_MAX - 1 };
    }

    // Check if window has expired
    if (now - data.windowStart >= RATE_LIMIT_WINDOW) {
      // Window expired - reset
      return { allowed: true, remaining: RATE_LIMIT_MAX - 1 };
    }

    // Check current count
    if (data.count >= RATE_LIMIT_MAX) {
      const retryAfter = RATE_LIMIT_WINDOW - (now - data.windowStart);
      return {
        allowed: false,
        remaining: 0,
        retryAfter
      };
    }

    return {
      allowed: true,
      remaining: RATE_LIMIT_MAX - data.count - 1
    };
  } catch (error) {
    // KV error - allow the request (fail open)
    console.error('Rate limit check error:', error);
    return { allowed: true, remaining: RATE_LIMIT_MAX };
  }
}

// Record a request for rate limiting
export async function recordRequest(
  kv: KVNamespace,
  clientIP: string
): Promise<void> {
  // Skip for localhost
  if (clientIP === 'localhost' || clientIP === '127.0.0.1') {
    return;
  }

  try {
    const key = getRateLimitKey(clientIP);
    const now = Math.floor(Date.now() / 1000);

    const data = await kv.get(key, 'json') as RateLimitEntry | null;

    if (!data || now - data.windowStart >= RATE_LIMIT_WINDOW) {
      // New window
      await kv.put(key, JSON.stringify({
        count: 1,
        windowStart: now
      }), {
        expirationTtl: RATE_LIMIT_WINDOW + 60 // Extra 60s buffer
      });
    } else {
      // Increment existing window
      await kv.put(key, JSON.stringify({
        count: data.count + 1,
        windowStart: data.windowStart
      }), {
        expirationTtl: RATE_LIMIT_WINDOW - (now - data.windowStart) + 60
      });
    }
  } catch (error) {
    console.error('Rate limit record error:', error);
    // Don't fail - just log error
  }
}
