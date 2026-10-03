/**
 * RomCloud Issue Relay - Cloudflare Worker
 *
 * Receives filtered bug reports from RomCloud app and creates GitHub Issues.
 *
 * Security:
 * - GitHub token stored only in Worker secrets (never in source code)
 * - All sensitive data filtered client-side AND server-side
 * - Deduplication via KV store (24h TTL)
 * - Rate limiting per IP (10 req/hour)
 *
 * @author RomCloud
 * @version 1.0.0
 */

// GitHub API wrapper
import { GitHubClient } from './github';
// Sensitive data filter
import { filterSensitiveData } from './filter';
// Deduplication logic
import { checkDeduplication, recordReport } from './dedupe';
// Rate limiter
import { checkRateLimit, recordRequest } from './rate-limit';

// Environment config
interface Env {
  GITHUB_TOKEN: string;
  GITHUB_REPO_OWNER: string;
  GITHUB_REPO_NAME: string;
  ISSUES_KV: KVNamespace;
  WORKER_VERSION: string;
  APP_NAME: string;
}

// Report payload schema
interface ReportPayload {
  deviceId: string;      // Hash format: RC-xxxxxxxx
  version: string;       // App version
  errorType: string;     // Error category
  errorMessage: string;   // Error description
  stackTrace?: string;   // Optional stack trace
  context?: string;      // Additional context
  labels?: string[];     // Optional labels
  timestamp: string;     // ISO timestamp
}

// Response types
interface SuccessResponse {
  success: true;
  message: string;
  issueNumber?: number;
}

interface ErrorResponse {
  success: false;
  error: string;
  code: string;
}

type ApiResponse = SuccessResponse | ErrorResponse;

// Validate payload schema
function validatePayload(payload: unknown): payload is ReportPayload {
  if (typeof payload !== 'object' || payload === null) {
    return false;
  }

  const p = payload as Record<string, unknown>;

  // Required fields
  if (typeof p.deviceId !== 'string' || !p.deviceId.match(/^RC-[a-f0-9]{8}$/)) {
    return false;
  }
  if (typeof p.version !== 'string' || p.version.length > 20) {
    return false;
  }
  if (typeof p.errorType !== 'string' || p.errorType.length > 100) {
    return false;
  }
  if (typeof p.errorMessage !== 'string' || p.errorMessage.length > 5000) {
    return false;
  }

  // Optional fields validation
  if (p.stackTrace !== undefined && typeof p.stackTrace !== 'string') {
    return false;
  }
  if (p.context !== undefined && typeof p.context !== 'string') {
    return false;
  }
  if (p.labels !== undefined && !Array.isArray(p.labels)) {
    return false;
  }
  if (p.timestamp !== undefined && typeof p.timestamp !== 'string') {
    return false;
  }

  return true;
}

// Build GitHub issue body
function buildIssueBody(report: ReportPayload, env: Env): string {
  const labels = report.labels || ['auto-reported', 'bug'];
  const severity = report.errorType.includes('CRASH') ? '🔴 Critical' : '🟡 Error';

  const body = `## ${severity} ${report.errorType}

**Reported from:** ${report.deviceId}
**App Version:** v${report.version}
**Timestamp:** ${report.timestamp || new Date().toISOString()}

### Error Message
\`\`\`
${report.errorMessage}
\`\`\`

${report.stackTrace ? `### Stack Trace
\`\`\`
${report.stackTrace}
\`\`\`
` : ''}

${report.context ? `### Additional Context
${report.context}
` : ''}

---
*🤖 Auto-reported from ${env.APP_NAME} v${report.version}*

<!-- reported_by=${report.deviceId} -->`

  return body;
}

// Main request handler
async function handleReport(request: Request, env: Env): Promise<ApiResponse> {
  // Only accept POST
  if (request.method !== 'POST') {
    return {
      success: false,
      error: 'Method not allowed',
      code: 'METHOD_NOT_ALLOWED'
    };
  }

  // Parse and validate JSON
  let payload: unknown;
  try {
    payload = await request.json();
  } catch {
    return {
      success: false,
      error: 'Invalid JSON payload',
      code: 'INVALID_JSON'
    };
  }

  // Validate schema
  if (!validatePayload(payload)) {
    return {
      success: false,
      error: 'Invalid payload schema',
      code: 'INVALID_SCHEMA'
    };
  }

  const report = payload as ReportPayload;

  // Check rate limit (10 requests per hour per IP)
  const clientIP = request.headers.get('CF-Connecting-IP') || 'unknown';
  const rateLimitResult = await checkRateLimit(env.ISSUES_KV, clientIP);
  if (!rateLimitResult.allowed) {
    return {
      success: false,
      error: `Rate limit exceeded. Try again in ${Math.ceil(rateLimitResult.retryAfter / 60)} minutes.`,
      code: 'RATE_LIMITED'
    };
  }
  await recordRequest(env.ISSUES_KV, clientIP);

  // Check deduplication (same error hash within 24h)
  const dedupeHash = `${report.deviceId}:${report.errorType}:${hashString(report.errorMessage.substring(0, 200))}`;
  const isDupe = await checkDeduplication(env.ISSUES_KV, dedupeHash);
  if (isDupe) {
    return {
      success: true,
      message: 'Duplicate report ignored (already reported within 24h)'
    };
  }

  // Filter any remaining sensitive data (belt and suspenders)
  const filteredMessage = filterSensitiveData(report.errorMessage);
  const filteredStack = report.stackTrace ? filterSensitiveData(report.stackTrace) : undefined;
  const filteredContext = report.context ? filterSensitiveData(report.context) : undefined;

  // Build issue
  const github = new GitHubClient(env.GITHUB_TOKEN);
  const title = `[Auto] ${report.errorType}: ${report.errorMessage.substring(0, 80)}${report.errorMessage.length > 80 ? '...' : ''}`;
  const body = buildIssueBody({
    ...report,
    errorMessage: filteredMessage,
    stackTrace: filteredStack,
    context: filteredContext
  }, env);

  // Create GitHub issue
  const issueResult = await github.createIssue(
    env.GITHUB_REPO_OWNER,
    env.GITHUB_REPO_NAME,
    title,
    body,
    report.labels || ['auto-reported', 'bug']
  );

  if (!issueResult.success) {
    return {
      success: false,
      error: issueResult.error || 'Failed to create GitHub issue',
      code: 'GITHUB_ERROR'
    };
  }

  // Record for deduplication (24h TTL)
  await recordReport(env.ISSUES_KV, dedupeHash, 86400);

  return {
    success: true,
    message: 'Report submitted successfully',
    issueNumber: issueResult.issueNumber
  };
}

// Simple hash function for deduplication
function hashString(str: string): string {
  let hash = 0;
  for (let i = 0; i < str.length; i++) {
    const char = str.charCodeAt(i);
    hash = ((hash << 5) - hash) + char;
    hash = hash & hash; // Convert to 32bit integer
  }
  return Math.abs(hash).toString(16);
}

// Health check endpoint
async function handleHealth(request: Request, env: Env): Promise<Response> {
  if (request.method !== 'GET') {
    return new Response('Method not allowed', { status: 405 });
  }

  return new Response(JSON.stringify({
    status: 'healthy',
    version: env.WORKER_VERSION,
    app: env.APP_NAME,
    timestamp: new Date().toISOString()
  }), {
    headers: { 'Content-Type': 'application/json' }
  });
}

// Main worker entry point
export default {
  async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    const url = new URL(request.url);

    // Health check endpoint
    if (url.pathname === '/health') {
      const response = await handleHealth(request, env);
      // Add CORS headers
      const headers = new Headers(response.headers);
      headers.set('Access-Control-Allow-Origin', '*');
      headers.set('Access-Control-Allow-Methods', 'GET, OPTIONS');
      return new Response(response.body, {
        status: response.status,
        headers
      });
    }

    // Report endpoint
    if (url.pathname === '/report' && request.method === 'POST') {
      // Check content type
      const contentType = request.headers.get('Content-Type');
      if (!contentType?.includes('application/json')) {
        return new Response(JSON.stringify({
          success: false,
          error: 'Content-Type must be application/json',
          code: 'INVALID_CONTENT_TYPE'
        }), {
          status: 400,
          headers: { 'Content-Type': 'application/json' }
        });
      }

      // Check content length (max 100KB)
      const contentLength = parseInt(request.headers.get('Content-Length') || '0');
      if (contentLength > 102400) {
        return new Response(JSON.stringify({
          success: false,
          error: 'Payload too large (max 100KB)',
          code: 'PAYLOAD_TOO_LARGE'
        }), {
          status: 413,
          headers: { 'Content-Type': 'application/json' }
        });
      }

      // Handle preflight CORS
      if (request.method === 'OPTIONS') {
        return new Response(null, {
          status: 204,
          headers: {
            'Access-Control-Allow-Origin': '*',
            'Access-Control-Allow-Methods': 'POST, OPTIONS',
            'Access-Control-Allow-Headers': 'Content-Type',
            'Access-Control-Max-Age': '86400'
          }
        });
      }

      const result = await handleReport(request, env);

      const headers = new Headers({
        'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*'
      });

      return new Response(JSON.stringify(result), {
        status: result.success ? 200 : 400,
        headers
      });
    }

    // 404 for other paths
    return new Response(JSON.stringify({
      success: false,
      error: 'Not found',
      code: 'NOT_FOUND'
    }), {
      status: 404,
      headers: { 'Content-Type': 'application/json' }
    });
  }
};
