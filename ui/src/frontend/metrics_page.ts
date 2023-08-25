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
  success,
} from '../base/result';
import {EngineProxy} from '../common/engine';
import {pluginManager, PluginManager} from '../common/plugins';
import {STR, NUM} from '../common/query_result';
import {raf} from '../core/raf_scheduler';
import {MetricVisualisation} from '../public';

import {globals} from './globals';
import {createPage} from './pages';
import {Select} from './widgets/select';
import {Spinner} from './widgets/spinner';
import {VegaView} from './widgets/vega_view';

type Format = 'json'|'prototext'|'proto';
// const FORMATS: Format[] = ['json', 'prototext', 'proto'];

function getEngine(): EngineProxy|undefined {
  const engineId = globals.getCurrentEngine()?.id;
  if (engineId === undefined) {
    return undefined;
  }
  const engine = globals.engines.get(engineId)?.getProxy('MetricsPage');
  return engine;
}

async function getMetrics(engine: EngineProxy): Promise<string[]> {
  const metrics: string[] = ["Select a slice name"];
  const results = await engine.query(
    "select distinct name from slices order by name"
  );

  for (const it = results.iter({ name: STR }); it.valid(); it.next()) {
    metrics.push(it.name);
  }
  return metrics;
}

type LatencyData = {
  summary: {
    min_dur_us: number;
    avg_dur_us: number;
    max_dur_us: number;
    count: number;
  };

  data: {
    dur_us: number;
  }[]
}

async function getMetric(
    engine: EngineProxy, metric: string, _format: Format): Promise<string> {

  let result = await engine.query(
    `select min(dur) / 1000 as min_dur, avg(dur) / 1000 as avg_dur, max(dur) / 1000 as max_dur from slices where name = "${metric}"`
  );

  const data: LatencyData = {
    summary: {
      min_dur_us: 0,
      avg_dur_us: 0,
      max_dur_us: 0,
      count: 0,
    },
    data: []
  };

  for (const it = result.iter({min_dur: NUM, avg_dur: NUM, max_dur: NUM}); it.valid(); it.next()) {
    data.summary.min_dur_us = it.min_dur;
    data.summary.avg_dur_us = it.avg_dur;
    data.summary.max_dur_us = it.max_dur;
    break; // should only have one row!
  }

  result = await engine.query(
    `select count(*) as cnt from slices where name = "${metric}"`
  );

  for (const it = result.iter({cnt: NUM}); it.valid(); it.next()) {
    data.summary.count = it.cnt;
    break; // should only have one row!
  }

  result = await engine.query(
    `select (dur / 1000) as dur_us from slices where name = "${metric}"`
  );

  for (const it = result.iter({dur_us: NUM}); it.valid(); it.next()) {
    data.data.push({dur_us: it.dur_us});
  }

  return JSON.stringify(data);
}

function spec(name: string): string {
  return JSON.stringify({
    "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
    "width": "container",
    "height": 300,
    "data": {"name": "metric"},
    "description": ".",
    "layer": [
      {
        "mark": {"type": "rule", "color": "red"},
        "encoding": {
          "x": {
            "aggregate": "max",
            "field": "dur_us",
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
              "binned": true,
              "step": 10
            },
            "field": "dur_us",
            "axis": {
              "title": `${name} Duration (us)`
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

class MetricsController {
  engine: EngineProxy;
  plugins: PluginManager;
  private _metrics: string[];
  private _selected?: string;
  private _result: Result<string>;
  private _format: Format;
  private _json: any;

  constructor(plugins: PluginManager, engine: EngineProxy) {
    this.plugins = plugins;
    this.engine = engine;
    this._metrics = [];
    this._result = success('');
    this._json = {};
    this._format = 'json';
    getMetrics(this.engine).then((metrics) => {
      this._metrics = metrics;
    });
  }

  get metrics(): string[] {
    return this._metrics;
  }

  get visualisations(): MetricVisualisation[] {
    // return this.plugins.metricVisualisations().filter(
    //     (v) => v.metric === this.selected);

    if (!this.selected) {
      return [];
    }

    return [
      {
        metric: this.selected,
        path: ["data"],
        spec: spec(this.selected),
      },
    ];
  }

  set selected(metric: string|undefined) {
    if (this._selected === metric) {
      return;
    }
    this._selected = metric;
    this.update();
  }

  get selected(): string|undefined {
    return this._selected;
  }

  set format(format: Format) {
    if (this._format === format) {
      return;
    }
    this._format = format;
    this.update();
  }

  get format(): Format {
    return this._format;
  }

  get result(): Result<string> {
    return this._result;
  }

  get resultAsJson(): any {
    return this._json;
  }

  private update() {
    const selected = this._selected;
    const format = this._format;
    if (selected === undefined) {
      this._result = success('');
      this._json = {};
    } else {
      this._result = pending();
      this._json = {};
      getMetric(this.engine, selected, format)
          .then((result) => {
            if (this._selected === selected && this._format === format) {
              this._result = success(result);
              if (format === 'json') {
                this._json = JSON.parse(result);
              }
            }
          })
          .catch((e) => {
            if (this._selected === selected && this._format === format) {
              this._result = error(e);
              this._json = {};
            }
          })
          .finally(() => {
            raf.scheduleFullRedraw();
          });
    }
    raf.scheduleFullRedraw();
  }
}

interface MetricResultAttrs {
  result: Result<string>;
  json: LatencyData;
}

class MetricResultView implements m.ClassComponent<MetricResultAttrs> {
  view({attrs}: m.CVnode<MetricResultAttrs>) {
    const result = attrs.result;
    if (isPending(result)) {
      return m(Spinner);
    }

    if (isError(result)) {
      return m('pre.metric-error', result.error);
    }

    return m('pre', JSON.stringify(attrs.json.summary, null, 2));
  }
}

interface MetricPickerAttrs {
  controller: MetricsController;
}

class MetricPicker implements m.ClassComponent<MetricPickerAttrs> {
  view({attrs}: m.CVnode<MetricPickerAttrs>) {
    const {controller} = attrs;
    return m(
        '.metrics-page-picker',
        m(Select,
          {
            value: controller.selected,
            oninput: (e: Event) => {
              if (!e.target) return;
              controller.selected = (e.target as HTMLSelectElement).value;
            },
          },
          controller.metrics.map(
              (metric) =>
                  m('option',
                    {
                      value: metric,
                      key: metric,
                    },
                    metric))),
        // m(
        //     Select,
        //     {
        //       oninput: (e: Event) => {
        //         if (!e.target) return;
        //         controller.format =
        //             (e.target as HTMLSelectElement).value as Format;
        //       },
        //     },
        //     FORMATS.map((f) => {
        //       return m('option', {
        //         selected: controller.format === f,
        //         key: f,
        //         value: f,
        //         label: f,
        //       });
        //     }),
        //     ),
    );
  }
}

interface MetricVizViewAttrs {
  visualisation: MetricVisualisation;
  data: any;
}

class MetricVizView implements m.ClassComponent<MetricVizViewAttrs> {
  view({attrs}: m.CVnode<MetricVizViewAttrs>) {
    return m(
        '',
        m(VegaView, {
          spec: attrs.visualisation.spec,
          data: {
            metric: attrs.data,
          },
        }),
    );
  }
};

class MetricPageContents implements m.ClassComponent {
  controller?: MetricsController;

  oncreate() {
    const engine = getEngine();
    if (engine !== undefined) {
      this.controller = new MetricsController(pluginManager, engine);
    }
  }

  view() {
    const controller = this.controller;
    if (controller === undefined) {
      return m('');
    }

    const json = controller.resultAsJson;

    return [
      m(MetricPicker, {
        controller,
      }),
      (controller.format === 'json') &&
          controller.visualisations.map((visualisation) => {
            let data = json;
            for (const p of visualisation.path) {
              data = data[p] ?? [];
            }
            return m(MetricVizView, {visualisation, data});
          }),
      m(MetricResultView, {result: controller.result, json: controller.resultAsJson as LatencyData}),
    ];
  }
}

export const MetricsPage = createPage({
  view() {
    return m('.metrics-page', m(MetricPageContents));
  },
});
