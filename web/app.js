const form = document.querySelector('#workbench');
const jobs = document.querySelector('#jobs');
const status = document.querySelector('#status');
const results = document.querySelector('#results');
const run = document.querySelector('#run');
const add = document.querySelector('#add');
const modes = {
  fcfs: 'FCFS', sjf: 'Shortest Job First', priority: 'Priority',
  rr: 'Round Robin', adaptive: 'Adaptive',
};
const columns = [
  ['arrival', 'Arrival', 0, 1000000000], ['priority', 'Priority', -1000000, 1000000],
  ['memory', 'Memory', 1, 1000000], ['deadline', 'Deadline', 0, 1000000000],
  ['bursts', 'Bursts'], ['pages', 'Pages'],
];
const example = [
  [0, 3, 12, 32, '9,75,5', '0,1,0,2'],
  [0, 1, 14, 0, '16', '0,1,2,1,3'],
  [2, 8, 18, 22, '4,36,5', '0,1,3'],
  [4, 5, 10, 40, '7,115,6', '0,1,2,3'],
  [6, 2, 12, 48, '10', '0,2,1'],
];

for (const field of ['modeA', 'modeB']) {
  const select = form.elements[field];
  for (const [value, name] of Object.entries(modes)) select.add(new Option(name, value));
}
form.elements.modeB.value = 'adaptive';

function element(type, text = '', className = '') {
  const node = document.createElement(type);
  node.textContent = text;
  node.className = className;
  return node;
}

function refreshRows() {
  Array.from(jobs.rows).forEach((row, i) => {
    const pid = 'P' + (i + 1);
    row.cells[0].textContent = pid;
    row.querySelectorAll('input').forEach((input, j) => {
      input.setAttribute('aria-label', pid + ' ' + columns[j][1]);
    });
    const remove = row.querySelector('button');
    remove.setAttribute('aria-label', 'Remove ' + pid);
    remove.disabled = jobs.rows.length === 1;
  });
  add.disabled = jobs.rows.length >= 100;
}

function addRow(values, focus = false) {
  const row = jobs.insertRow();
  row.append(element('th'));
  row.cells[0].scope = 'row';
  columns.forEach(([key, , min, max], i) => {
    const input = element('input');
    input.dataset.key = key;
    input.required = key !== 'pages';
    if (min !== undefined) {
      input.type = 'number';
      input.min = min;
      input.max = max;
    } else {
      input.placeholder = key === 'bursts' ? '4,75,6' : '0,1,2';
    }
    input.value = values[i];
    row.insertCell().append(input);
  });
  const remove = element('button', '×', 'remove');
  remove.type = 'button';
  remove.addEventListener('click', () => {
    const neighbor = row.nextElementSibling || row.previousElementSibling;
    row.remove();
    refreshRows();
    neighbor?.querySelector('input').focus();
    markEdited();
  });
  row.insertCell().append(remove);
  if (focus) row.querySelector('input').focus();
}

function loadExample() {
  jobs.replaceChildren();
  example.forEach(values => addRow(values));
  refreshRows();
}

function table(headers, rows) {
  const output = element('table');
  const heading = output.createTHead().insertRow();
  for (const text of headers) {
    const cell = element('th', text);
    cell.scope = 'col';
    heading.append(cell);
  }
  const body = output.createTBody();
  for (const values of rows) {
    const row = body.insertRow();
    for (const value of values) row.insertCell().textContent = value ?? '—';
  }
  return output;
}

function render(runs, names) {
  const summaries = runs.map(result => result.summary);
  document.querySelector('#results-note').textContent =
    'Processes completed · A ' + summaries[0].Completed + ' · B ' + summaries[1].Completed;

  const legend = document.querySelector('#legend');
  legend.replaceChildren(...names.map((name, i) => {
    const label = element('span');
    label.append(element('span', i ? 'B' : 'A', 'chip ' + (i ? 'b' : 'a')), name);
    return label;
  }));

  const charts = document.querySelector('#charts');
  charts.replaceChildren();
  for (const key of ['Average response', 'Average waiting', 'Average turnaround', 'Deadline misses',
    'Starvation events', 'CPU utilization', 'Memory utilization', 'I/O utilization']) {
    const chart = element('article', '', 'chart');
    chart.append(element('h3', key));
    const values = summaries.map(summary => summary[key]);
    const numbers = values.map(parseFloat);
    const max = key.endsWith('utilization') ? 100 : Math.max(1, ...numbers);
    values.forEach((value, i) => {
      const color = i ? 'b' : 'a';
      const line = element('div', '', 'barline');
      const bar = element('div', '', 'track');
      bar.setAttribute('aria-hidden', 'true');
      const fill = element('span', '', color);
      fill.style.width = (100 * numbers[i] / max) + '%';
      bar.append(fill);
      line.append(element('span', color.toUpperCase()), bar,
        element('strong', key === 'Memory utilization' ? numbers[i].toFixed(2) + '%' : value));
      chart.append(line);
    });
    charts.append(chart);
  }

  document.querySelector('#metrics').replaceChildren(table(['Metric', ...names],
    Object.keys(summaries[0]).map(key => [key, ...summaries.map(summary => summary[key])])));
  const processes = document.querySelector('#processes');
  processes.replaceChildren(...runs.map((result, i) => {
    const group = element('section', '', 'process-group');
    const wrapper = element('div', '', 'scroll');
    wrapper.append(table(['PID', 'State', 'Arrival', 'Finish', 'Wait', 'Turnaround', 'Response', 'I/O wait'],
      result.processes.slice().sort((a, b) => a[0] - b[0]).map(row => ['P' + row[0], ...row.slice(1)])));
    group.append(element('h3', names[i]), wrapper);
    return group;
  }));
  results.hidden = false;
}

let revision = 0;
function markEdited() {
  revision++;
  if (!results.hidden) {
    status.textContent = 'Inputs changed. Run again to update results.';
    status.className = '';
  }
}

form.addEventListener('input', markEdited);
add.addEventListener('click', () => {
  addRow([0, 1, 8, 0, '5', ''], true);
  refreshRows();
  markEdited();
});
document.querySelector('#demo').addEventListener('click', () => {
  loadExample();
  const defaults = {
    memory: 64, frames: 4, quantum: 4, starvation: 20, io: 'sstf',
    fit: 'first', paging: 'fifo', modeA: 'fcfs', modeB: 'adaptive',
  };
  for (const [key, value] of Object.entries(defaults)) form.elements[key].value = value;
  markEdited();
});
form.addEventListener('submit', async event => {
  event.preventDefault();
  const formData = new FormData(form);
  const settings = Object.fromEntries(['memory', 'frames', 'quantum', 'starvation', 'io', 'fit', 'paging']
    .map(key => [key, form.elements[key].type === 'number' ? Number(formData.get(key)) : formData.get(key)]));
  const workload = Array.from(jobs.rows, row => Object.fromEntries(
    Array.from(row.querySelectorAll('input'), input => [
      input.dataset.key, input.type === 'number' ? input.valueAsNumber : input.value,
    ])));
  const modeA = formData.get('modeA');
  const modeB = formData.get('modeB');
  const snapshot = revision;
  status.className = '';
  status.textContent = 'Running both policies…';
  run.disabled = true;
  try {
    const response = await fetch('/api/compare', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ settings, jobs: workload, modeA, modeB }),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error);
    render(data, [modes[modeA], modes[modeB]]);
    status.textContent = snapshot === revision ? 'Comparison complete.' : 'Inputs changed. Run again to update results.';
  } catch (error) {
    status.className = 'error';
    status.textContent = error instanceof TypeError ? 'Could not reach the local scheduler.' : error.message;
  } finally {
    run.disabled = false;
  }
});

loadExample();
form.requestSubmit();
