import * as Comlink from 'comlink';
export async function initWorker(){
  const w = new Worker(new URL('./analyzer.worker.ts', import.meta.url),{type:'module'});
  const api = Comlink.wrap<any>(w);
  await api.start?.();
  return api;
}
