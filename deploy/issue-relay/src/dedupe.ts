/**
 * Deduplication Module
 *
 * Prevents duplicate issues by tracking report hashes in KV store.
 * Uses 24-hour TTL to allow re-reporting after a day.
 */

interface KVResult {
  found: boolean;
  value?: string;
}

// Generate a hash key from report content
export function generateReportHash(
  deviceId: string,
  errorType: string,
  errorMessage: string
): string {
  // Use first 200 chars of message for hashing
  const messageKey = errorMessage.substring(0, 200);
  return `report:${deviceId}:${errorType}:${simpleHash(messageKey)}`;
}

// Simple string hash (not cryptographic, just for KV key)
function simpleHash(str: string): string {
  let hash = 0;
  for (let i = 0; i < str.length; i++) {
    const char = str.charCodeAt(i);
    hash = ((hash << 5) - hash) + char;
    hash = hash & hash;
  }
  return Math.abs(hash).toString(16);
}

// Check if a report was already submitted (within TTL window)
export async function checkDeduplication(
  kv: KVNamespace,
  hashKey: string
): Promise<boolean> {
  try {
    const existing = await kv.get(hashKey, 'text');
    return existing !== null;
  } catch {
    // KV error - allow the report (better to create dupe than lose report)
    console.error('KV read error:', error);
    return false;
  }
}

// Record a new report in KV store
export async function recordReport(
  kv: KVNamespace,
  hashKey: string,
  ttlSeconds: number = 86400 // 24 hours default
): Promise<boolean> {
  try {
    await kv.put(hashKey, JSON.stringify({
      timestamp: Date.now(),
      reported: true
    }), {
      expirationTtl: ttlSeconds
    });
    return true;
  } catch (error) {
    console.error('KV write error:', error);
    return false;
  }
}

// Get deduplication stats (for debugging)
export async function getDedupeStats(kv: KVNamespace): Promise<{
  totalReports: number;
  recentReports: number;
}> {
  try {
    const list = await kv.list({ prefix: 'report:' });
    const oneDayAgo = Date.now() - (24 * 60 * 60 * 1000);
    let recentCount = 0;

    // Count recent reports
    for (const key of list.keys) {
      const value = await kv.get(key.name, 'json') as { timestamp?: number } | null;
      if (value?.timestamp && value.timestamp > oneDayAgo) {
        recentCount++;
      }
    }

    return {
      totalReports: list.keys.length,
      recentReports: recentCount
    };
  } catch (error) {
    console.error('KV stats error:', error);
    return { totalReports: 0, recentReports: 0 };
  }
}
