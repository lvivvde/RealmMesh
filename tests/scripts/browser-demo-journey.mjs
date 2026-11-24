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
  const registration={account:'camp_guest',password:'camp-password'};
  assert.equal((await a('register',registration)).registered,true);
  await assert.rejects(()=>a('register',{...registration,password:'different-password'}),/注册未完成/);
  const alpha = await join(a, registration.account, registration.password, 'SceneAlpha');
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
    const registrationPage = await browser.newPage();
    await registrationPage.goto('http://'+host+'/');
    await registrationPage.getByRole('button',{name:'注册账号'}).click();
    const form = registrationPage.locator('#register');
    await form.locator('[name=account]').fill('camp_ui_guest');
    await form.locator('[name=password]').fill('ui-demo-password');
    await form.locator('[name=confirm]').fill('ui-demo-password');
    await form.getByRole('button',{name:'创建账号'}).click();
    await registrationPage.getByText('注册成功，请输入口令登录。').waitFor();
    await registrationPage.getByPlaceholder('口令',{exact:true}).fill('ui-demo-password');
    await registrationPage.getByRole('button',{name:'登录营地'}).click();
    await registrationPage.getByRole('heading',{name:'选择出行角色'}).waitFor();
    await registrationPage.close();
    const joinPage = async (account, password, name) => {
      const page = await browser.newPage({viewport:{width:1000,height:900}});
      const errors = [];
      page.on('pageerror', err => errors.push(err.message));
      let latest;
      const steps=[];
      page.on('websocket', socket => {
        socket.on('framesent', frame=>{
          const action=JSON.parse(frame.payload.toString());
          if(action.op==='move')steps.push(action);
        });
        socket.on('framereceived', frame => {
          const value = JSON.parse(frame.payload.toString());
          if (value.members) latest = value;
        });
      });
      await page.goto('http://'+host+'/');
      await page.getByPlaceholder('演示账号').fill(account);
      await page.getByPlaceholder('口令',{exact:true}).fill(password);
      await page.getByRole('button', {name:'登录营地'}).click();
      await page.getByRole('button', {name}).click();
      await page.locator('#world').waitFor({state:'visible'});
      return {page, state:()=>latest, errors, steps};
    };
    const a = await joinPage('camp_guest', 'camp-password', 'SceneAlpha');
    await a.page.getByRole('button',{name:'物品栏'}).click();
    assert.equal(await a.page.locator('#slots span').count(),18);
    await a.page.getByRole('button',{name:'地图'}).click();
    await a.page.locator('#dots circle').first().waitFor({state:'visible'});
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
    await a.page.keyboard.press('Escape');
    await a.page.keyboard.press('F9');
    assert.equal(await a.page.locator('#bag').isVisible(),true);
    await a.page.keyboard.press('Escape');
    const field=await a.page.locator('#world canvas').boundingBox();
    const own=()=>a.state().members.find(m=>m.id===a.state().self);
    const start={...own()};
    await a.page.mouse.click(field.x+field.width*.76,field.y+field.height*.68,{button:'right'});
    await waitUntil(()=>own().x>start.x && own().y>start.y);
    assert.ok(a.steps.some(step=>step.dx===2 && step.dy>0));
    await waitUntil(()=>b.state().members.some(m=>m.id===start.id && m.x>start.x && m.y>start.y));
    await a.page.mouse.click(field.x+field.width*.54,field.y+field.height*.52);
    await waitUntil(()=>a.steps.some(step=>step.dx<0 && Math.abs(step.dx)===1));
    await delay(900);
    const landed={...own()};
    await delay(500);
    assert.deepEqual(own(),landed);
    const mouse=a.page.mouse;
    await mouse.move(field.x+field.width*.88,field.y+field.height*.7);
    await mouse.down();await waitUntil(()=>own().x>landed.x);
    await mouse.move(field.x+field.width*.12,field.y+field.height*.7);
    const turning=own().x;
    await waitUntil(()=>own().x<turning);
    await mouse.up();await delay(350);
    const stopped={...own()};await delay(500);assert.deepEqual(own(),stopped);
  } finally { await browser.close(); }
}
