export interface DomMutation {
  selector: string;
  type: 'added' | 'removed' | 'text';
  ts: number;
}
export interface NetCall {
  url: string;
  method: string;
  status: number;
  jsonShape?: any;
  ts: number;
}
export interface UserAction {
  type: 'click' | 'input' | 'submit' | 'change';
  selector: string;
  value?: string;
  ts: number;
}

export type SiteEvent =
  | { kind: 'dom',    data: DomMutation }
  | { kind: 'net',    data: NetCall }
  | { kind: 'action', data: UserAction }
  | { kind: 'schema', data: { url: string; schema: any } };
