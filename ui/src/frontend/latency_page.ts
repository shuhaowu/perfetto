// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';

import {
  error,
  isError,
  isPending,
  pending,
  Result,
  ResultStatus,
  success,
} from '../base/result';
import { EngineProxy } from '../common/engine';
import { NUM, STR } from '../common/query_result';
import { raf } from '../core/raf_scheduler';
import { MetricVisualisation } from '../public';

import { globals } from './globals';
import { createPage } from './pages';
import { Select } from './widgets/select';
import { Spinner } from './widgets/spinner';
import { VegaView } from './widgets/vega_view';

type TimeUnit = "ns" | "us" | "ms" | "s"
const TIME_UNITS: TimeUnit[] = ["ns", "us", "ms", "s"];
const TIME_UNITS_DIVIDER: Map<TimeUnit, number> = new Map([
  ["ns", 1],
  ["us", 1000],
  ["ms", 1000000],
  ["s", 1000000000],
])

function getEngine(): EngineProxy | undefined {
  const engineId = globals.getCurrentEngine()?.id;
  if (engineId === undefined) {
    return undefined;
  }
  const engine = globals.engines.get(engineId)?.getProxy('LatencyPage');
  return engine;
}

async function getSlices(engine: EngineProxy): Promise<string[]> {
  const slices: string[] = [];

  const result = await engine.query("select distinct name from slices order by name");
  for (const it = result.iter({ name: STR }); it.valid(); it.next()) {
    slices.push(it.name);
  }

  return slices;
}

type LatencyData = {
  summary: {
    min_dur: number;
    avg_dur: number;
    max_dur: number;
    count: number;
  };

  data: { dur: number }[];
};

async function getLatency(engine: EngineProxy, slice: string, unit: TimeUnit): Promise<LatencyData> {
  const divider = TIME_UNITS_DIVIDER.get(unit)!;

  let result = await engine.query(`select (min(dur) / ${divider}) as min_dur, (avg(dur) / ${divider}) as avg_dur, (max(dur) / ${divider}) as max_dur, count(id) as cnt from slices where name = "${slice}"`);

  const data: LatencyData = {
    summary: {
      min_dur: 0,
      avg_dur: 0,
      max_dur: 0,
      count: 0,
    },
    data: [],
  };

  for (const it = result.iter({
    min_dur: NUM,
    avg_dur: NUM,
    max_dur: NUM,
    cnt: NUM,
  }); it.valid(); it.next()) {
    data.summary.min_dur = it.min_dur;
    data.summary.avg_dur = it.avg_dur;
    data.summary.max_dur = it.max_dur;
    data.summary.count = it.cnt;
  }

  result = await engine.query(`select (dur / ${divider}) as d from slices where name = "${slice}"`);

  for (const it = result.iter({ d: NUM }); it.valid(); it.next()) {
    data.data.push({ dur: it.d });
  }

  return data;
}

function spec(name: string, unit: string, maxbins: number = 100): string {
  return JSON.stringify({
    "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
    "width": "container",
    "height": 300,
    "data": { "name": "latencies" },
    "description": ".",
    "layer": [
      {
        "mark": { "type": "rule", "color": "red" },
        "encoding": {
          "x": {
            "aggregate": "max",
            "field": "dur",
            "type": "quantitative"
          }
        }
      },
      {
        "mark": {
          "type": "bar",
          "clip": true
        },
        "encoding": {
          "x": {
            "bin": {
              "maxbins": maxbins,
              "minstep": 1,
            },
            "field": "dur",
            "axis": {
              "title": `${name} duration (${unit})`
            }
          },
          "y": {
            "aggregate": "count",
            "scale": {
              "type": "symlog",
              "nice": false
            },
            "axis": {
              "title": "Count"
            }
          }
        }
      }
    ]
  });
}

class LatencyController {
  engine: EngineProxy;

  private _slices: string[];

  private _selected?: string;
  private _result: Result<LatencyData | undefined>;
  private _time_unit: TimeUnit;

  constructor(engine: EngineProxy) {
    this.engine = engine;
    this._slices = [];
    this._result = success(undefined);
    this._time_unit = 'us';
    getSlices(this.engine).then((slices) => {
      this._slices = slices;
    });
  }

  get slices(): string[] {
    return this._slices;
  }

  set selected(metric: string | undefined) {
    if (this._selected === metric) {
      return;
    }
    this._selected = metric;
    this.update();
  }

  get selected(): string | undefined {
    return this._selected;
  }

  set time_unit(unit: TimeUnit) {
    if (this._time_unit === unit) {
      return;
    }

    this._time_unit = unit;
    this.update();
  }

  get time_unit(): TimeUnit {
    return this._time_unit;
  }

  get result(): Result<LatencyData | undefined> {
    return this._result;
  }

  private update() {
    const selected = this._selected;
    const time_unit = this._time_unit;
    if (selected === undefined) {
      this._result = success(undefined);
    } else {
      this._result = pending();
      getLatency(this.engine, selected, time_unit)
        .then((result) => {
          if (this._selected === selected && this._time_unit === time_unit) {
            this._result = success(result);
          }
        })
        .catch((e) => {
          if (this._selected === selected && this._time_unit === time_unit) {
            console.error(e);
            this._result = error(e);
          }
        })
        .finally(() => {
          raf.scheduleFullRedraw();
        });
    }
    raf.scheduleFullRedraw();
  }
}

interface LatencyResultAttrs {
  result: Result<LatencyData | undefined>;
}

class LatencyResultView implements m.ClassComponent<LatencyResultAttrs> {
  view({ attrs }: m.CVnode<LatencyResultAttrs>) {
    const result = attrs.result;
    if (isPending(result)) {
      return m(Spinner);
    }

    if (isError(result)) {
      return m('pre.metric-error', result.error);
    }

    if (result.data) {
      return m('pre', JSON.stringify(result.data.summary, null, 2));
    }

    return m("pre", "No data. Select a slice to see the data.");
  }
}

interface LatencyPickerAttrs {
  controller: LatencyController;
}

class LatencyPicker implements m.ClassComponent<LatencyPickerAttrs> {
  view({ attrs }: m.CVnode<LatencyPickerAttrs>) {
    const { controller } = attrs;
    return m(
      '.metrics-page-picker',
      m("span", "Select a slice: "),
      m(Select,
        {
          value: controller.selected,
          oninput: (e: Event) => {
            if (!e.target) return;
            controller.selected = (e.target as HTMLSelectElement).value;
          },
        },
        controller.slices.map(
          (slice) =>
            m('option',
              {
                value: slice,
                key: slice,
              },
              slice))),
      m(
        Select,
        {
          oninput: (e: Event) => {
            if (!e.target) return;
            controller.time_unit =
              (e.target as HTMLSelectElement).value as TimeUnit;
          },
        },
        TIME_UNITS.map((unit) => {
          return m('option', {
            selected: controller.time_unit === unit,
            key: unit,
            value: unit,
            label: unit,
          });
        }),
      ),
    );
  }
}

interface LatencyVizViewAttrs {
  visualisation: MetricVisualisation;
  latencies: { dur: number }[];
}

class LatencyVizView implements m.ClassComponent<LatencyVizViewAttrs> {
  view({ attrs }: m.CVnode<LatencyVizViewAttrs>) {
    return m(
      '',
      m(VegaView, {
        spec: attrs.visualisation.spec,
        data: {
          latencies: attrs.latencies,
        },
      }),
    );
  }
};

class LatencyPageContents implements m.ClassComponent {
  controller?: LatencyController;

  oncreate() {
    const engine = getEngine();
    if (engine !== undefined) {
      this.controller = new LatencyController(engine);
    }
  }

  view() {
    const controller = this.controller;
    if (controller === undefined) {
      return m('');
    }

    const result = controller.result;
    const components = [];
    if (controller.selected && result.status === ResultStatus.SUCCESS && result.data != undefined) {
      const latencies = result.data.data;
      const s = spec(controller.selected, controller.time_unit);
      components.push(m(
        LatencyVizView, {
        visualisation: {
          metric: controller.selected,
          path: [],
          spec: s,
        },
        latencies,
      }));
    }

    return [
      m(LatencyPicker, {
        controller,
      }),
      ...components,
      m(LatencyResultView, { result }),
    ];
  }
}

export const LatencysPage = createPage({
  view() {
    return m('.metrics-page', m(LatencyPageContents));
  },
});
