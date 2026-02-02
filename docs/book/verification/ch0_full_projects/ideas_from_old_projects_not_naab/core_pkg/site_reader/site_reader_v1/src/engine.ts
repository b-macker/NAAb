import { EventEmitter } from 'events';
import { SchemaWhisperer } from './jsonSchema';
import { SemanticExtractor } from './domSemantic';
import type { SiteEvent } from './schema';

export interface SiteReaderOptions { maxMutations?: number }

export class SiteReader extends EventEmitter {
  private observer?: MutationObserver;
  private opts: Required<SiteReaderOptions>;
  constructor(opts: SiteReaderOptions = {}) {
    super();
    this.opts = { maxMutations: opts.maxMutations ?? 500 };
  }

  start() {
    this.observeDOM();
    this.interceptNetwork();
    this.captureActions();
  }
  stop() {
    this.observer?.disconnect();
    this.restore();
  }

  // DOM observation ----------------------------------------------------------
  private observeDOM() {
    let count = 0;
    this.observer = new MutationObserver(recs => {
      recs.forEach(r => {
        if (count++ % this.opts.maxMutations !== 0) return;
        Array.from(r.addedNodes).forEach(n => {
          if (!(n instanceof HTMLElement)) return;
          this.emitEv({ kind:'dom', data:{selector:this.css(n), type:'added', ts:Date.now()} });
          if (n.tagName === 'TABLE') {
            const tJSON = SemanticExtractor.tableToJSON(n as HTMLTableElement);
            this.emit('table', tJSON);
          }
        });
      });
    });
    this.observer.observe(document, {childList:true,subtree:true});
  }
  private css(el: Element): string {
    const path=[];
    while(el && path.length<4){
      let sel = el.nodeName.toLowerCase();
      if(el.id) sel+='#'+el.id;
      else if(el.className) sel+='.'+el.className.split(' ')[0];
      path.unshift(sel); el=el.parentElement!;
    }
    return path.join('>');
  }

  // Network interception -----------------------------------------------------
  private origXHR:{open:any,send:any}|null=null;
  private origFetch:any=null;
  private interceptNetwork() {
    const self = this;

    // XHR
    this.origXHR = { open: XMLHttpRequest.prototype.open, send: XMLHttpRequest.prototype.send };
    XMLHttpRequest.prototype.open = function(m:string,u:string,...rest:any[]){
      (this as any)._sr_m = m; (this as any)._sr_u = u;
      return self.origXHR!.open.apply(this,[m,u,...rest]);
    };
    XMLHttpRequest.prototype.send = function(body?:any){
      this.addEventListener('load',()=>{
        const url=(this as any)._sr_u, method=(this as any)._sr_m;
        const ev:SiteEvent={kind:'net',data:{url,method,status:this.status,ts:Date.now()}};
        self.emitEv(ev);
        if(/application\/json/.test(this.getResponseHeader('content-type')||'')){
          const schema = SchemaWhisperer.inferSchema(this.responseText);
          schema && self.emitEv({kind:'schema',data:{url,schema}});
        }
      });
      return self.origXHR!.send.apply(this,[body]);
    };

    // fetch
    this.origFetch = window.fetch.bind(window);
    window.fetch = async (input:RequestInfo,init?:RequestInit)=>{
      const res = await self.origFetch(input,init);
      const clone=res.clone();
      const ct=clone.headers.get('content-type')||'';
      const url = typeof input==='string'?input:clone.url;
      self.emitEv({kind:'net',data:{url,method:init?.method||'GET',status:clone.status,ts:Date.now()}});
      if(/application\/json/.test(ct)){
        try{
          const txt = await clone.text();
          const schema = SchemaWhisperer.inferSchema(txt);
          schema && self.emitEv({kind:'schema',data:{url,schema}});
        }catch{}
      }
      return res;
    };
  }
  private restore(){
    if(this.origXHR){
      XMLHttpRequest.prototype.open=this.origXHR.open;
      XMLHttpRequest.prototype.send=this.origXHR.send;
    }
    if(this.origFetch) window.fetch=this.origFetch;
  }

  // Actions ------------------------------------------------------------------
  private captureActions(){
    document.addEventListener('click',e=>{
      const t=e.target as HTMLElement;
      this.emitEv({kind:'action',data:{type:'click',selector:this.css(t),ts:Date.now()}});
    },true);
    document.addEventListener('input',e=>{
      const t=e.target as HTMLInputElement;
      this.emitEv({kind:'action',data:{type:'input',selector:this.css(t),value:t.value,ts:Date.now()}});
    },true);
  }

  private emitEv(ev:SiteEvent){
    this.emit(ev.kind, ev.data);
    this.emit('event', ev);
  }
}
