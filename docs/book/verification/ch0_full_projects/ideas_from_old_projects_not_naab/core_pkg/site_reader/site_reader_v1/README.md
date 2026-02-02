# @site-reader/core

Offline, headless site reader that streams DOM, network, and user-action events.

## TL;DR

```bash
npm install
npm run build
```

### Browser

```js
import { SiteReader } from '@site-reader/core';
const sr = new SiteReader();
sr.on('event', e => console.log(e));
sr.start();
```

### Worker

```js
import { initWorker } from '@site-reader/core';
const sr = await initWorker();
sr.on('net', n => console.log(n));
```
