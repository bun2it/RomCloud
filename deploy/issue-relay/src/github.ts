/**
 * GitHub API Client
 *
 * Handles all GitHub API interactions for the issue relay.
 * Token is passed from environment - never hardcoded.
 */

interface GitHubIssueResult {
  success: boolean;
  issueNumber?: number;
  error?: string;
}

export class GitHubClient {
  private token: string;
  private baseUrl = 'https://api.github.com';

  constructor(token: string) {
    this.token = token;
  }

  private async request<T>(
    method: string,
    path: string,
    body?: unknown
  ): Promise<{ status: number; data: T }> {
    const headers: Record<string, string> = {
      'Authorization': `Bearer ${this.token}`,
      'Accept': 'application/vnd.github.v3+json',
      'User-Agent': 'RomCloud-IssueRelay/1.0',
      'X-GitHub-Api-Version': '2022-11-28'
    };

    const options: RequestInit = { method, headers };

    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
      options.body = JSON.stringify(body);
    }

    const response = await fetch(`${this.baseUrl}${path}`, options);
    const data = await response.json() as T;

    return { status: response.status, data };
  }

  async createIssue(
    owner: string,
    repo: string,
    title: string,
    body: string,
    labels: string[]
  ): Promise<GitHubIssueResult> {
    try {
      // First check if repo exists and is accessible
      const repoCheck = await this.request<{ id: number; full_name: string }>(
        'GET',
        `/repos/${owner}/${repo}`
      );

      if (repoCheck.status !== 200) {
        return {
          success: false,
          error: `Repository not accessible (${repoCheck.status})`
        };
      }

      // Create issue
      const issue = await this.request<{ number: number; html_url: string }>(
        'POST',
        `/repos/${owner}/${repo}/issues`,
        {
          title,
          body,
          labels
        }
      );

      if (issue.status === 201) {
        return {
          success: true,
          issueNumber: issue.data.number
        };
      }

      // Handle rate limiting
      if (issue.status === 403) {
        const rateLimitRemaining = issue.headers?.['x-ratelimit-remaining'];
        if (rateLimitRemaining === '0') {
          return {
            success: false,
            error: 'GitHub API rate limit exceeded. Please try again later.'
          };
        }
      }

      return {
        success: false,
        error: `GitHub API error: ${issue.status}`
      };
    } catch (error) {
      return {
        success: false,
        error: `Network error: ${error instanceof Error ? error.message : 'Unknown error'}`
      };
    }
  }
}
