import dayjs from 'dayjs'

const BASE_URL = '/api/v4'
const TOKEN = 'LKfxGaKpK-xtgzDj5K6B'

const headers = { 'PRIVATE-TOKEN': TOKEN }

async function fetchAll(url) {
  let results = []
  let page = 1
  while (true) {
    const sep = url.includes('?') ? '&' : '?'
    const res = await fetch(`${BASE_URL}${url}${sep}per_page=100&page=${page}`, { headers })
    if (!res.ok) break
    const data = await res.json()
    if (!Array.isArray(data) || !data.length) break
    results = results.concat(data)
    // GitLab 14.0 commits endpoint doesn't return x-total-pages header
    // so we check: if we got a full page, there might be more
    const totalPages = res.headers.get('x-total-pages')
    if (totalPages) {
      if (page >= parseInt(totalPages)) break
    } else {
      // No header: if less than 100 results, we've reached the end
      if (data.length < 100) break
    }
    page++
  }
  return results
}

export async function getProjects() {
  const all = await fetchAll('/projects?simple=true&order_by=last_activity_at')
  // 过滤掉空仓库（没有 default_branch 说明是空的）
  return all.filter(p => p.default_branch)
}

export async function getUsers() {
  const users = await fetchAll('/users?active=true')
  return users.map(u => ({
    id: u.id,
    name: u.name,
    username: u.username,
    email: u.email || u.public_email || '',
    avatar: u.name.split(' ').map(w => w[0]).join('').slice(0, 2).toUpperCase() || u.username.slice(0, 2).toUpperCase(),
    avatarUrl: u.avatar_url,
    role: u.is_admin ? 'Admin' : 'Developer',
    color: stringToColor(u.username),
  }))
}

export async function getAllCommits(projects, startDate, endDate, onProgress) {
  const since = dayjs(startDate).startOf('day').toISOString()
  const until = dayjs(endDate).endOf('day').toISOString()
  const allCommits = []
  let done = 0

  // 先按最近活跃排序，跳过在时间范围前就不活跃的项目
  const startTs = dayjs(startDate).valueOf()
  const activeProjects = projects.filter(p => {
    if (!p.last_activity_at) return true
    return dayjs(p.last_activity_at).valueOf() >= startTs
  })

  if (onProgress) onProgress(0, activeProjects.length)

  // 提高并发到 20
  const concurrency = 20
  for (let i = 0; i < activeProjects.length; i += concurrency) {
    const batch = activeProjects.slice(i, i + concurrency)
    const promises = batch.map(async (project) => {
      try {
        // 1. Get all branches for this project
        const branches = await fetchAll(`/projects/${project.id}/repository/branches`)
        const branchNames = branches.map(b => b.name)
        if (!branchNames.length) return []

        // 2. 并发拉取所有分支的 commits（而不是串行）
        const seen = new Set()
        const projectCommits = []

        const branchPromises = branchNames.map(branch =>
          fetchAll(
            `/projects/${project.id}/repository/commits?ref_name=${encodeURIComponent(branch)}&since=${since}&until=${until}&with_stats=true`
          ).catch(() => [])
        )
        const branchResults = await Promise.all(branchPromises)

        for (let bi = 0; bi < branchResults.length; bi++) {
          for (const c of branchResults[bi]) {
            if (seen.has(c.id)) continue
            // 跳过 Merge Commit（审核合并产生的提交，非实际代码编写）
            if (isMergeCommit(c.title, c.message)) continue
            seen.add(c.id)
            projectCommits.push({
              projectId: project.id,
              projectName: project.name,
              sha: c.id,
              shortId: c.short_id,
              title: c.title,
              message: c.message || '',
              authorName: c.author_name,
              authorEmail: c.author_email,
              date: dayjs(c.committed_date).format('YYYY-MM-DD'),
              hour: dayjs(c.committed_date).hour(),
              weekday: dayjs(c.committed_date).day(),
              additions: c.stats?.additions || 0,
              deletions: c.stats?.deletions || 0,
              isBugFix: isBugCommit(c.title, c.message),
              isFeature: isFeatureCommit(c.title, c.message),
              commitType: getCommitType(c.title, c.message),
              branch: branchNames[bi],
            })
          }
        }
        return projectCommits
      } catch {
        return []
      } finally {
        done++
        if (onProgress) onProgress(done, activeProjects.length)
      }
    })
    const results = await Promise.all(promises)
    for (const r of results) allCommits.push(...r)
  }
  return allCommits
}

function isMergeCommit(title, message) {
  const text = `${title}`.toLowerCase()
  // 规范中的 merge/sync 类型
  if (/^(merge|sync)(\(.+\))?: /i.test(text)) return true
  return /^merge\s+(branch|remote|pull|request|tag|commit)/i.test(text) ||
    /^合并分支/i.test(text) ||
    /^merge\s+'/i.test(text) ||
    /^merge\s+"/i.test(text)
}

function isBugCommit(title, message) {
  const text = `${title} ${message}`.toLowerCase()
  // 优先匹配规范格式：fix(...): 或 to(...):
  if (/^(fix|to)(\(.+\))?: /i.test(title)) return true
  return /\b(fix|bug|hotfix|patch|issue|resolve|修复|修改|问题|缺陷)\b/i.test(text)
}

function isFeatureCommit(title, message) {
  const text = `${title} ${message}`.toLowerCase()
  // 优先匹配规范格式：feat(...):
  if (/^feat(\(.+\))?: /i.test(title)) return true
  return /\b(feat|feature|add|new|implement|新增|添加|功能|实现)\b/i.test(text)
}

// 识别重构类型
function isRefactorCommit(title) {
  return /^refactor(\(.+\))?: /i.test(title)
}

// 识别性能优化
function isPerfCommit(title) {
  return /^perf(\(.+\))?: /i.test(title)
}

// 识别文档变更
function isDocsCommit(title) {
  return /^docs(\(.+\))?: /i.test(title)
}

// 识别代码格式
function isStyleCommit(title) {
  return /^style(\(.+\))?: /i.test(title)
}

// 识别测试
function isTestCommit(title) {
  return /^test(\(.+\))?: /i.test(title)
}

// 识别构建/工具
function isChoreCommit(title) {
  return /^(chore|ci|build)(\(.+\))?: /i.test(title)
}

// 获取 commit 的分类标签
function getCommitType(title, message) {
  if (isBugCommit(title, message)) return 'bugfix'
  if (isFeatureCommit(title, message)) return 'feature'
  if (isRefactorCommit(title)) return 'refactor'
  if (isPerfCommit(title)) return 'perf'
  if (isDocsCommit(title)) return 'docs'
  if (isStyleCommit(title)) return 'style'
  if (isTestCommit(title)) return 'test'
  if (isChoreCommit(title)) return 'chore'
  return 'other'
}

function stringToColor(str) {
  const colors = ['#6366f1','#ec4899','#f59e0b','#10b981','#8b5cf6','#ef4444','#06b6d4','#f97316','#14b8a6','#a855f7']
  let hash = 0
  for (let i = 0; i < str.length; i++) hash = str.charCodeAt(i) + ((hash << 5) - hash)
  return colors[Math.abs(hash) % colors.length]
}

export function aggregateByUser(commits, users, userIds = []) {
  const filtered = userIds.length
    ? commits.filter(c => userIds.includes(findUserId(c.authorName, c.authorEmail, users)))
    : commits

  const map = {}
  for (const c of filtered) {
    const uid = findUserId(c.authorName, c.authorEmail, users)
    const key = uid || c.authorEmail || c.authorName
    if (!map[key]) {
      const user = users.find(u => u.id === uid)
      map[key] = {
        id: uid || key,
        name: user?.name || c.authorName,
        avatar: user?.avatar || c.authorName.slice(0, 2).toUpperCase(),
        avatarUrl: user?.avatarUrl,
        role: user?.role || '外部提交者',
        color: user?.color || stringToColor(c.authorName),
        totalCommits: 0, bugFixes: 0, features: 0, refactors: 0,
        additions: 0, deletions: 0, activeDays: new Set(),
      }
    }
    map[key].totalCommits += 1
    const t = c.commitType || 'other'
    if (t === 'bugfix') map[key].bugFixes += 1
    else if (t === 'feature') map[key].features += 1
    else map[key].refactors += 1
    map[key].additions += c.additions
    map[key].deletions += c.deletions
    map[key].activeDays.add(c.date)
  }

  return Object.values(map)
    .map(u => ({ ...u, activeDays: u.activeDays.size }))
    .sort((a, b) => b.totalCommits - a.totalCommits)
}

export function aggregateByDate(commits, users, userIds = []) {
  const filtered = userIds.length
    ? commits.filter(c => userIds.includes(findUserId(c.authorName, c.authorEmail, users)))
    : commits

  const map = {}
  for (const c of filtered) {
    if (!map[c.date]) {
      map[c.date] = { date: c.date, commits: 0, bugFixes: 0, features: 0, refactors: 0 }
    }
    map[c.date].commits += 1
    const t = c.commitType || 'other'
    if (t === 'bugfix') map[c.date].bugFixes += 1
    else if (t === 'feature') map[c.date].features += 1
    else map[c.date].refactors += 1
  }
  return Object.values(map).sort((a, b) => a.date.localeCompare(b.date))
}

export function getTotals(commits, users, userIds = []) {
  const filtered = userIds.length
    ? commits.filter(c => userIds.includes(findUserId(c.authorName, c.authorEmail, users)))
    : commits

  return filtered.reduce((acc, c) => {
    const t = c.commitType || 'other'
    return {
      commits: acc.commits + 1,
      bugFixes: acc.bugFixes + (t === 'bugfix' ? 1 : 0),
      features: acc.features + (t === 'feature' ? 1 : 0),
      refactors: acc.refactors + (t === 'refactor' ? 1 : 0),
      perfs: acc.perfs + (t === 'perf' ? 1 : 0),
      docs: acc.docs + (t === 'docs' ? 1 : 0),
      styles: acc.styles + (t === 'style' ? 1 : 0),
      tests: acc.tests + (t === 'test' ? 1 : 0),
      chores: acc.chores + (t === 'chore' ? 1 : 0),
      others: acc.others + (t === 'other' ? 1 : 0),
      additions: acc.additions + c.additions,
      deletions: acc.deletions + c.deletions,
    }
  }, { commits: 0, bugFixes: 0, features: 0, refactors: 0, perfs: 0, docs: 0, styles: 0, tests: 0, chores: 0, others: 0, additions: 0, deletions: 0 })
}

function findUserId(name, email, users) {
  const nameLower = (name || '').toLowerCase().trim()
  const emailLower = (email || '').toLowerCase().trim()

  // 1. Exact match on name/username
  let u = users.find(u =>
    u.name === name ||
    u.username === name ||
    u.name.toLowerCase() === nameLower ||
    u.username.toLowerCase() === nameLower
  )
  if (u) return u.id

  // 2. Email match
  if (emailLower) {
    u = users.find(u => u.email && u.email.toLowerCase() === emailLower)
    if (u) return u.id

    // 3. Email prefix match (before @) against username
    const emailPrefix = emailLower.split('@')[0]
    if (emailPrefix) {
      u = users.find(u => u.username.toLowerCase() === emailPrefix)
      if (u) return u.id
    }
  }

  // 4. Fuzzy: commit author contains GitLab name or vice versa
  if (nameLower.length >= 2) {
    u = users.find(u =>
      (u.name.toLowerCase().includes(nameLower) || nameLower.includes(u.name.toLowerCase())) ||
      (u.username.toLowerCase().includes(nameLower) || nameLower.includes(u.username.toLowerCase()))
    )
    if (u) return u.id
  }

  return null
}
