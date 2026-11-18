import assert from 'node:assert/strict';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';

const serverFile = new URL('../../tools/browser-demo/server.mjs', import.meta.url);
const WebSocket = createRequire(serverFile)('ws');
const host = '127.0.0.1:' + process.argv[6];
const bridge = spawn(process.execPath, [fileURLToPath(serverFile), ...process.argv.slice(2, 6)], {
  env: { ...process.env, REALMMESH_DEMO_SELF_SIGNED: '1', REALMMESH_DEMO_PORT:process.argv[6] },
  stdio: ['ignore', 'pipe', 'inherit']
});
const clients = [];
const signal = AbortSignal.timeout(55000);
async function connect() {
  const ws = new WebSocket('ws://'+host+'/play', {origin:'http://'+host});
  clients.push(ws);
  await once(ws, 'open', {signal});
  return async (op, args = {}) => {
    let response = once(ws, 'message', {signal});
    ws.send(JSON.stringify({op, ...args}));
    let msg = JSON.parse((await response)[0]);
    while ('position' in msg) {
      response = once(ws, 'message', {signal});
      msg = JSON.parse((await response)[0]);
    }
    assert.equal(msg.error, undefined, msg.error);
    return msg;
  };
}
async function join(send, account, password, name) {
  assert.deepEqual((await send('login', {account,password})).characters, []);
  const {characters} = await send('create', {name});
  assert.equal(characters[0].name, name);
  return (await send('select', {id:characters[0].id})).self;
}
async function observe(send, id, x, y) {
  for (let attempt = 0; attempt < 30; attempt++) {
    const player = (await send('state')).members.find(member => member.id === id);
    if (player?.x === x && player.y === y) return;
    await delay(50);
  }
  assert.fail('另一窗口未观察到已确认落点');
}
try {
  await Promise.race([once(bridge.stdout, 'data', {signal}), once(bridge, 'exit', {signal}).then(()=>{throw Error('bridge exited')})]);
  const a = await connect(), b = await connect();
  const alpha = await join(a, 'journey', 'journey-secret', 'SceneAlpha');
  const beta = await join(b, 'scene-second', 'second-secret', 'SceneBeta');
  await observe(a,beta,0,0);
  await observe(b,alpha,0,0);
  await a('move',{dir:'right'});
  await observe(b,alpha,1,0);
  await b('move',{dir:'down'});
  await observe(a,beta,0,1);
  if (process.env.REALMMESH_DEMO_PLAYWRIGHT) {
    for (const ws of clients) ws.terminate();
    await runBrowserJourney();
  }
} finally {
  for (const ws of clients) ws.terminate();
  bridge.kill();
}

async function runBrowserJourney() {
  const { chromium } = createRequire(import.meta.url)(process.env.REALMMESH_DEMO_PLAYWRIGHT);
  const browser = await chromium.launch({
    executablePath: process.env.REALMMESH_DEMO_CHROME,
    args: ['--enable-unsafe-swiftshader','--disable-background-timer-throttling',
           '--disable-renderer-backgrounding','--disable-backgrounding-occluded-windows']
  });
  try {
    const joinPage = async (account, password, name) => {
      const page = await browser.newPage({viewport:{width:1000,height:900}});
      const errors = [];
      page.on('pageerror', err => errors.push(err.message));
      let latest;
      page.on('websocket', socket => {
        socket.on('framereceived', frame => {
          const value = JSON.parse(frame.payload.toString());
          if (value.members) latest = value;
        });
      });
      await page.goto('http://'+host+'/');
      await page.getByPlaceholder('演示账号').fill(account);
      await page.getByPlaceholder('口令').fill(password);
      await page.getByRole('button', {name:'登录营地'}).click();
      await page.getByRole('button', {name}).click();
      await page.locator('#world').waitFor({state:'visible'});
      return {page, state:()=>latest, errors};
    };
    const a = await joinPage('journey', 'journey-secret', 'SceneAlpha');
    const b = await joinPage('scene-second', 'second-secret', 'SceneBeta');
    async function waitUntil(predicate) {
      for(let i=0;i<100;i++) { if(predicate()) return; await delay(70); }
      assert.fail('页面场景未同步');
    }
    for(const [actor, observer, axis] of [[a,b,'x'],[b,a,'y']]) {
      const id = actor.state().self;
      const locate = participant => participant.state()?.members.find(member=>member.id===id)?.[axis];
      await waitUntil(()=>locate(observer)!==undefined);
      const before = locate(observer);
      const pad = await actor.page.locator('#stick').boundingBox();
      const x=pad.x+pad.width/2, y=pad.y+pad.height/2;
      const mouse = actor.page.mouse;
      await mouse.move(x,y); await mouse.down();
      await mouse.move(x+(axis==='x'?32:0), y+(axis==='y'?32:0));
      await waitUntil(()=>locate(observer)>before);
      await mouse.up();
      await delay(500);
      const stopped = locate(actor);
      await delay(650);
      assert.equal(locate(actor),stopped);
      assert.equal(locate(observer),stopped);
      assert.deepEqual(actor.errors,[]);
    }
  } finally { await browser.close(); }
}
