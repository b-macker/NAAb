import { SiteReader } from './engine';
import * as Comlink from 'comlink';

const reader = new SiteReader();
const api = {
  start(){ reader.start(); },
  stop(){ reader.stop(); },
  on(kind:string, cb:(data:any)=>void){
    reader.on(kind, Comlink.proxy(cb));
  }
};
Comlink.expose(api);
