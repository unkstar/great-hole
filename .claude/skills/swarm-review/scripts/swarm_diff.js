#!/usr/bin/env node
/**
 * swarm_diff.js - 用 Playwright 抓取 Swarm review 的 diff 内容
 * 用法: node swarm_diff.js <review_url> <user> <ticket>
 */

const { chromium } = require('playwright');

async function fetchSwarmDiff(reviewUrl, user, ticket) {
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    httpCredentials: { username: user, password: ticket }
  });

  try {
    const page = await context.newPage();

    // 先访问 API 设置认证
    console.error(`Fetching review metadata...`);
    const urlMatch = reviewUrl.match(/(https?:\/\/[^/]+)\/reviews\/(\d+)/);
    if (!urlMatch) throw new Error('Invalid review URL');
    const baseUrl = urlMatch[1];
    const reviewId = urlMatch[2];

    // 用 API 获取文件列表
    const filesResp = await page.goto(
      `${baseUrl}/api/v10/reviews/${reviewId}/files`,
      { waitUntil: 'networkidle' }
    );
    const filesJson = await filesResp.json();
    const files = filesJson?.data?.files || [];

    console.error(`Found ${files.length} files`);

    // 访问 review 页面获取 diff
    await page.goto(reviewUrl, { waitUntil: 'networkidle', timeout: 30000 });

    // 等待 diff 内容加载
    await page.waitForTimeout(3000);

    // 尝试抓取 diff 内容（Swarm 用 React 渲染）
    const diffContent = await page.evaluate(() => {
      // 找所有 diff 相关元素
      const results = [];

      // 文件标题
      const fileHeaders = document.querySelectorAll('.diff-header, .file-header, [class*="fileName"], [class*="file-name"]');
      fileHeaders.forEach(el => results.push('=== ' + el.textContent.trim() + ' ==='));

      // diff 行内容
      const diffLines = document.querySelectorAll('.diff-line, [class*="diff"], .added, .deleted, .context');
      diffLines.forEach(el => {
        const text = el.textContent;
        if (text.trim()) results.push(text);
      });

      // 如果上面没抓到，尝试抓整个 review body
      if (results.length === 0) {
        const body = document.querySelector('#swarm-body-container, .review-wrapper, main');
        if (body) return [body.innerText.substring(0, 5000)];
      }

      return results;
    });

    await browser.close();

    if (diffContent.length === 0) {
      console.error('No diff content found via DOM. Swarm uses dynamic rendering.');
      // 输出页面 title 确认登录成功
      return { reviewId, baseUrl, files, diff: null };
    }

    return { reviewId, baseUrl, files, diff: diffContent.join('\n') };

  } catch (err) {
    await browser.close();
    throw err;
  }
}

const [,, reviewUrl, user, ticket] = process.argv;
if (!reviewUrl || !user || !ticket) {
  console.error('Usage: node swarm_diff.js <review_url> <user> <ticket>');
  process.exit(1);
}

fetchSwarmDiff(reviewUrl, user, ticket)
  .then(result => {
    console.log(JSON.stringify(result, null, 2));
  })
  .catch(err => {
    console.error('Error:', err.message);
    process.exit(1);
  });
