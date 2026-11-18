import * as T from "./three.module.js";
let renderer, camera, scene, ground;
const actors = new Map();
const body = new T.CylinderGeometry(0.22, 0.32, 0.65, 6),
  head = new T.IcosahedronGeometry(0.23);
const material = (color) =>
  new T.MeshStandardMaterial({ color, flatShading: true });
const gold = material(0xe9b557),
  blue = material(0x5a83c2),
  skin = material(0xf7d7b3);
function init() {
  const canvas = document.querySelector("canvas");
  renderer = new T.WebGLRenderer({ canvas, antialias: true });
  renderer.setSize(800, 420, false);
  scene = new T.Scene();
  scene.background = new T.Color(0xadc9bd);
  camera = new T.OrthographicCamera(-9, 9, 4.725, -4.725, 0.1, 80);
  ground = new T.Group();
  scene.add(ground);
  const floor = new T.Mesh(new T.PlaneGeometry(60, 60), material(0x83ac90));
  floor.rotation.x = -Math.PI / 2;
  ground.add(floor);
  const grid = new T.GridHelper(60, 60, 0x709d85, 0x709d85);
  grid.position.y = 0.01;
  ground.add(grid);
  const tower = new T.ConeGeometry(0.75, 2, 5),
    pine = material(0x416e62);
  for (const [x, z] of [
    [-4, -2],
    [-5, -3],
    [6, -3],
    [7, -2],
    [-3, 6],
    [4, 8],
  ]) {
    const tree = new T.Mesh(tower, pine);
    tree.position.set(x, 1, z);
    scene.add(tree);
  }
  scene.add(new T.HemisphereLight(0xfff3cf, 0x82968d, 2));
  const sun = new T.DirectionalLight(0xffedd2, 2);
  sun.position.set(-5, 9, 3);
  scene.add(sun);
}
export function draw({ self, members }) {
  if (!renderer) init();
  const visible = new Set(members.map((member) => member.id));
  for (const [id, actor] of actors)
    if (!visible.has(id)) {
      scene.remove(actor);
      actors.delete(id);
    }
  for (const member of members) {
    let actor = actors.get(member.id);
    if (!actor) {
      actor = new T.Group();
      const coat = new T.Mesh(body, member.id === self ? gold : blue);
      coat.position.y = 0.35;
      const face = new T.Mesh(head, skin);
      face.position.y = 0.88;
      actor.add(coat, face);
      actors.set(member.id, actor);
      scene.add(actor);
    }
    const together = members
      .filter((other) => other.x === member.x && other.y === member.y)
      .map((other) => other.id)
      .sort();
    const offset =
      (together.indexOf(member.id) - (together.length - 1) / 2) * 0.65;
    actor.position.set(member.x + offset, 0, member.y);
    if (member.id === self) {
      camera.position.set(member.x, 10, member.y + 12);
      camera.lookAt(member.x, 0, member.y);
      ground.position.set(member.x, 0, member.y);
    }
  }
  renderer.render(scene, camera);
}
