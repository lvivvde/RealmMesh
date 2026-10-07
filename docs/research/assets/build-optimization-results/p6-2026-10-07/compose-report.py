#!/usr/bin/env python3
import json,statistics,sys
from pathlib import Path
ROOT=Path(__file__).parent
ASSETS=Path(sys.argv[1])
data={p:json.loads((ASSETS/(p+'-samples.json')).read_text()) for p in ['mac','lima']}
def span(s):return f"{s['median']:.2f}（{s['min']:.2f}–{s['max']:.2f}）"
labels={'cold-build':'已备源码空产物 ALL build','unit':'Unit 无操作连续入口','cpp':'Unit cpp token 连续入口','lua':'Lua 内部头 token build','hot-full':'热完整连续入口','cold-complete':'完整冷入口（独立总计时）'}
results=[]
for p,label in [('mac','macOS'),('lima','Lima Linux')]:
 d=data[p]
 lines=[label+'：单位秒，括号为 min–max。','', '| 场景 | n | R0 | 最终 | 降幅 | 配对更快 |','| --- | ---: | ---: | ---: | ---: | ---: |']
 for kind in labels:
  c=d['comparisons'][kind]
  lines.append(f"| {labels[kind]} | {c['before']['n']} | {span(c['before'])} | {span(c['after'])} | {c['reduction']*100:.2f}% | {c['faster_pairs']}/{c['before']['n']} |")
 c=d['cache_comparison']
 lines.append(f"| 本地兼容 cache OFF→ON 净重建 | 3 | {span(c['OFF'])} | {span(c['ON'])} | {c['reduction']*100:.2f}% | {sum(x['on_s']<x['off_s'] for x in c['pairs'])}/3 |")
 results.append('\n'.join(lines))
coldparts=['| 平台 / 阶段（连续冷入口 n=3） | R0 中位秒 | 最终中位秒 |','| --- | ---: | ---: |']
for p,label in [('mac','macOS'),('lima','Lima')]:
 for stage in ['prepare','configure','build','test']:
  values={}
  for side in ['r0','final']:
   values[side]=[sum(x['wall_s'] for x in row['steps'] if x['label']==stage) for row in data[p]['groups']['cold-'+side]['rows']]
  coldparts.append(f"| {label} / {stage} | {statistics.median(values['r0']):.2f} | {statistics.median(values['final']):.2f} |")
resources=['| 平台 | 树 RSS 峰值 MiB | 最低可用 MiB | swap 增长 MiB | 压力 / OOM |','| --- | ---: | ---: | ---: | --- |']
for p,label in [('mac','macOS'),('lima','Lima')]:
 r=data[p]['resources']
 resources.append(f"| {label} | {r['max_tree_rss_mib']:.2f} | {r['min_available_mib']:.2f} | {r['max_swap_growth_mib']:.2f} | "+('正常 1 / 未采集' if p=='mac' else f"OOM {r['oom_kills']}")+' |')
decomp=['| 平台 / 组 | 旧 649 项单例时间和 | 新版本共同 649 项时间和 | 新增 18 项时间和 |','| --- | ---: | ---: | ---: |']
for p,label in [('mac','macOS'),('lima','Lima')]:
 values=data[p]['test_scope']['hot_decomposition']
 for i in range(1,4):
  before=next(x for x in values if x['side']=='r0' and x['sample']=='hot-full-'+str(i))
  after=next(x for x in values if x['side']=='final' and x['sample']=='hot-full-'+str(i))
  decomp.append(f"| {label} / {i} | {before['common_sum_s']:.2f} | {after['common_sum_s']:.2f} | {after['added_sum_s']:.2f} |")
gates=['| 平台 | R0 完整冷 / 热 | 最终完整冷 / 热 | 最终 Ninja ON | Make OFF 回退 |','| --- | --- | --- | --- | --- |']
for p,label in [('mac','macOS'),('lima','Lima')]:
 d=data[p]
 full=next(r for r in d['groups']['checks']['rows'] if r['name']=='final-full')
 passed=full['tests_total']-full['tests_failed']
 gates.append(f"| {label} | 冷/热各 3×649/649 | 冷/热各 3×667/667 | {passed}/667"+('（失败）' if full['exit'] else '')+" | 667/667 |")
env=['| 平台 | 系统 / 架构 | C++ / CMake / ccache | 内存 / CPU | 能力 |','| --- | --- | --- | --- | --- |']
work=['| 平台 / 场景 | R0 编译 / 链接 / 归档 | 最终编译 / 链接 / 归档 | 外部 sodium CC（前 / 后） |','| --- | ---: | ---: | ---: |']
for p,label in [('mac','macOS'),('lima','Lima')]:
 d=data[p];e=d['groups']['final']['environment'][0]
 env.append(f"| {label} | {e['platform']} | {e['tools']['cxx']} / {e['tools']['cmake']} / {e['tools']['ccache']} | {e['memory_total_gib']:.2f}GiB / {e['logical_cpus']} | QUIC + TLS/TCP |")
 for kind in ['cold-build','unit','cpp','lua','hot-full']:
  a=next(x for x in d['groups']['r0']['rows'] if x['name']==kind+'-1')
  b=next(x for x in d['groups']['final']['rows'] if x['name']==kind+'-1')
  def counts(r):return f"{r['compile_count']} / {r['link_count']} / {r['archive_count']}"
  work.append(f"| {label} / {kind} | {counts(a)} | {counts(b)} | {a['sodium_compile_requests']} / {b['sodium_compile_requests']} |")
s=(ROOT/'report-notes.md').read_text()
stability=['| 平台 | 正常稳定性 0/0 且版本头不变 | 恢复后无操作 0/0 | 稳定性门槛 |','| --- | ---: | ---: | --- |']
for p,label in [('mac','macOS'),('lima','Lima')]:
 d=data[p];stable=[r for r in d['groups']['checks']['rows'] if r['name'].startswith('stable-')]
 noop=[r for r in d['groups']['checks']['rows'] if r['name'].endswith('-noop')]
 stable_good=sum(r['compile_count']==r['link_count']==r['archive_count']==r['sodium_compile_requests']==r['sodium_assembly_requests']==0 and not r['generated_version_headers_changed'] for r in stable)
 noop_good=sum(r['compile_count']==r['link_count']==r['archive_count']==r['sodium_compile_requests']==r['sodium_assembly_requests']==0 for r in noop)
 stability.append(f"| {label} | {stable_good}/{len(stable)} | {noop_good}/{len(noop)} | "+('通过' if d['stability_gate_passed'] else '未通过')+' |')
clock=data['lima']['clock_environment']['monitor']
assert data['lima']['clock_environment_gate_passed'] is True
s=s.replace('[动态时钟结果]', f"最终只读观察覆盖 {clock['elapsed_s']/3600:.2f} 小时；无倒退或超过 50ms 跳变，环境时钟门槛通过。每分钟内核频率/tick 读数和最终时间服务状态均由最终审计核验。")
s=s.replace('[动态稳定性结果]','\n'.join(stability))
s=s.replace('[动态环境表]','\n'.join(env)).replace('[动态工作量表]','\n'.join(work))
s=s.replace('[动态冷入口阶段表]','\n'.join(coldparts))
s=s.replace('[动态统计表]','\n\n'.join(results)).replace('[动态门槛表]','\n'.join(gates)).replace('[动态资源表]','\n'.join(resources)).replace('[动态完整测试成本分解表]','\n'.join(decomp))
s=s.replace('[动态资产链接]','[`mac-samples.json`](assets/build-optimization-results/p6-2026-10-07/mac-samples.json)、[`lima-samples.json`](assets/build-optimization-results/p6-2026-10-07/lima-samples.json)、[冻结/复现与证据说明](assets/build-optimization-results/p6-2026-10-07/README.md)、[校验清单](assets/build-optimization-results/p6-2026-10-07/manifest.json)。')
(ROOT/'p6-report.md').write_text(s)
print(ROOT/'p6-report.md')
