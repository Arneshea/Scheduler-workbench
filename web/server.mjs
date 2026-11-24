import { createServer } from 'node:http';
import { execFile } from 'node:child_process';
import { access, readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const host = '127.0.0.1';
const port = 4173;
const binary = fileURLToPath(new URL(process.platform === 'win32' ? '../scheduler.exe' : '../scheduler', import.meta.url));
const url = 'http://' + host + ':' + port;
const openBrowser = process.platform === 'win32' && process.argv.includes('--open');
if (openBrowser) {
  try {
    await access(binary);
  } catch {
    console.error('Build scheduler.exe first. See README.md for the build command.');
    process.exit(1);
  }
}
const assets = new Map([
  ['/', ['index.html', 'text/html']],
  ['/style.css', ['style.css', 'text/css']],
  ['/app.js', ['app.js', 'text/javascript']],
]);
const modes = ['fcfs', 'sjf', 'priority', 'rr', 'adaptive'];

function integer(value, min, max, label) {
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new Error(label + ' must be an integer from ' + min + ' to ' + max + '.');
  }
  return value;
}

function choice(value, allowed, label) {
  if (!allowed.includes(value)) throw new Error('Invalid ' + label + '.');
  return value;
}

function sequence(value, label) {
  if (typeof value !== 'string' || !/^\d+(,\s*\d+)*$/.test(value.trim())) {
    throw new Error(label + ' needs comma-separated whole numbers.');
  }
  return value.trim().split(',').map(Number);
}

function prepare(input) {
  const { settings, jobs, modeA, modeB } = input;
  const memory = integer(settings.memory, 1, 1000000, 'Memory');
  const commands = [
    'set memory ' + memory,
    'set frames ' + integer(settings.frames, 1, 10000, 'Page frames'),
    'set quantum ' + integer(settings.quantum, 1, 1000000, 'Quantum'),
    'set starvation ' + integer(settings.starvation, 1, 1000000, 'Starvation threshold'),
    'set fit ' + choice(settings.fit, ['first', 'best', 'worst'], 'memory fit'),
    'set paging ' + choice(settings.paging, ['fifo', 'lru'], 'page replacement'),
    'set io ' + choice(settings.io, ['fcfs', 'sstf', 'scan'], 'disk policy'),
  ];
  if (!Array.isArray(jobs) || !jobs.length || jobs.length > 100) {
    throw new Error('Enter between 1 and 100 processes.');
  }
  let totalCpu = 0;
  jobs.forEach((job, index) => {
    const label = 'P' + (index + 1) + ' ';
    const bursts = sequence(job.bursts, label + 'bursts');
    if (bursts.length % 2 === 0) throw new Error(label + 'bursts must end with a CPU duration.');
    bursts.forEach((value, i) => {
      if (i % 2) integer(value, 0, 199, label + 'disk track');
      else totalCpu += integer(value, 1, 1000000, label + 'CPU duration');
    });
    const pages = job.pages.trim() ? sequence(job.pages, label + 'pages') : [];
    pages.forEach(value => integer(value, 0, 1000000, label + 'page'));
    commands.push([
      'add', integer(job.arrival, 0, 1000000000, label + 'arrival'),
      integer(job.priority, -1000000, 1000000, label + 'priority'),
      integer(job.memory, 1, memory, label + 'memory'),
      integer(job.deadline, 0, 1000000000, label + 'deadline'),
      bursts.join(','), ...(pages.length ? [pages.join(',')] : []),
    ].join(' '));
  });
  if (totalCpu > 250000) throw new Error('Keep the workload at or below 250,000 total CPU ticks.');
  return [modeA, modeB].map(mode => {
    choice(mode, modes, 'CPU mode');
    return [...commands, 'set cpu ' + (mode === 'adaptive' ? 'fcfs' : mode),
      'set adaptive ' + (mode === 'adaptive' ? 'on' : 'off'), 'run', 'quit', ''].join('\n');
  });
}

function run(commands) {
  return new Promise((resolve, reject) => {
    const child = execFile(binary, { timeout: 15000, windowsHide: true }, (error, stdout, stderr) => {
      if (error?.code === 'ENOENT') {
        reject(new Error('Scheduler executable is missing. Build it using the README, then retry.'));
      } else if (error) {
        reject(new Error(stderr.trim() || 'The simulator did not finish. Try a smaller workload.'));
      } else {
        const lines = stdout.trim().split(/\r?\n/);
        if (!lines.some(line => line.startsWith('Elapsed ticks:'))) {
          reject(new Error('The scheduler returned an unexpected report.'));
          return;
        }
        const summary = {};
        const processes = [];
        for (const line of lines.slice(2)) {
          if (/^\d+\s/.test(line)) {
            processes.push(line.trim().split(/\s+/).map((field, i) => i === 1 ? field : Number(field)));
          } else {
            for (const field of line.split(' | ')) {
              const colon = field.indexOf(': ');
              if (colon >= 0) summary[field.slice(0, colon)] = field.slice(colon + 2);
            }
          }
        }
        resolve({ summary, processes });
      }
    });
    child.stdin.on('error', () => {});
    child.stdin.end(commands);
  });
}

let busy = false;
createServer(async (request, response) => {
  const json = (status, data) => {
    response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
    response.end(JSON.stringify(data));
  };
  if (request.method === 'POST' && request.url === '/api/compare') {
    if (request.headers['content-type'] !== 'application/json') return json(415, { error: 'Expected JSON.' });
    if (busy) return json(429, { error: 'A comparison is already running. Try again shortly.' });
    busy = true;
    try {
      let size = 0;
      let body = '';
      for await (const chunk of request.setEncoding('utf8')) {
        size += chunk.length;
        if (size > 128000) {
          json(413, { error: 'Workload is too large.' });
          return;
        }
        body += chunk;
      }
      let commands;
      try {
        commands = prepare(JSON.parse(body));
      } catch (error) {
        return json(400, { error: error instanceof TypeError ? 'Invalid comparison data.' : error.message });
      }
      const results = await Promise.allSettled(commands.map(run));
      const failure = results.find(result => result.status === 'rejected');
      if (failure) return json(500, { error: failure.reason.message });
      return json(200, results.map(result => result.value));
    } catch {
      if (!response.destroyed) return json(400, { error: 'Could not read comparison data.' });
    } finally {
      busy = false;
    }
  }
  const asset = request.method === 'GET' && assets.get(request.url);
  if (!asset) return json(404, { error: 'Page not found.' });
  try {
    const content = await readFile(new URL(asset[0], import.meta.url));
    response.writeHead(200, { 'Content-Type': asset[1] + '; charset=utf-8', 'X-Content-Type-Options': 'nosniff' });
    response.end(content);
  } catch {
    json(500, { error: 'Could not load the page.' });
  }
}).listen(port, host, () => {
  console.log('Scheduler Workbench: ' + url);
  if (openBrowser) {
    execFile('explorer.exe', [url], { windowsHide: true }, () => {});
  }
});
