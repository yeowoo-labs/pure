import http from 'node:http';
import { MAX_PROJECT_BYTES, parseProject } from './web/project.mjs';

const maxRevision = 9223372036854775807n;
function revisionText(value, allowZero = false) {
  if (typeof value !== 'string' || !/^(0|[1-9]\d*)$/.test(value) ||
      value.length > 19 || BigInt(value) > maxRevision || (!allowZero && value === '0'))
    throw new Error('revision은 범위 안의 정수 문자열이어야 합니다.');
  return value;
}

export function remoteTarget(base, key) {
  const url = new URL(base);
  if (url.protocol !== 'http:' || !['127.0.0.1', 'localhost'].includes(url.hostname) ||
      url.username || url.password || url.pathname !== '/' || url.search || url.hash)
    throw new Error('현재 연결 호스트는 로컬 http://127.0.0.1:PORT 형식을 사용합니다.');
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(key)) throw new Error('작업물 이름은 영문·숫자·_·- 1~64자입니다.');
  url.hostname = '127.0.0.1';
  url.pathname = `/objects/${key}`;
  return url;
}

function request(url, method, body, headers = {}) {
  return new Promise((resolve, reject) => {
    let done = false;
    const finish = (error, value) => {
      if (done) return;
      done = true;
      clearTimeout(deadline);
      if (error) reject(error); else resolve(value);
    };
    const req = http.request(url, { method, agent: false, headers: { ...headers,
      'Content-Length': body?.byteLength ?? 0, Connection: 'close' } }, (res) => {
      const chunks = [];
      let size = 0;
      res.on('data', (chunk) => {
        size += chunk.length;
        if (size > MAX_PROJECT_BYTES) {
          const error = new Error('서버 작업물이 허용 크기를 넘었습니다.');
          finish(error);
          res.destroy(error);
          req.destroy(error);
        } else chunks.push(chunk);
      });
      res.on('error', (error) => finish(error));
      res.on('aborted', () => finish(new Error('응답이 완료되기 전에 연결이 끊겼습니다.')));
      res.on('end', () => {
        if (res.statusCode !== 200) {
          const error = new Error(`원격 작업 실패: ${res.statusCode} ${res.headers['x-pure-error'] ?? ''}`.trim());
          error.status = res.statusCode;
          error.etag = res.headers.etag;
          finish(error);
          return;
        }
        try {
          const match = /^"([1-9]\d*)"$/.exec(res.headers.etag ?? '');
          if (!match) throw new Error('서버의 revision 응답이 올바르지 않습니다.');
          const revision = revisionText(match[1]);
          finish(null, { revision, bytes: Buffer.concat(chunks, size) });
        } catch (error) { finish(error); }
      });
    });
    const deadline = setTimeout(() => {
      const error = new Error('원격 작업 시간 초과: 저장 완료 여부는 다시 확인해야 합니다.');
      finish(error);
      req.destroy(error);
    }, 12000);
    req.on('error', (error) => finish(error));
    req.end(body);
  });
}

export async function pushProject(target, expected, project) {
  revisionText(expected, true);
  const text = JSON.stringify(project);
  parseProject(text);
  const body = Buffer.from(text, 'utf8');
  const headers = expected === '0' ? { 'If-None-Match': '*' } : { 'If-Match': `"${expected}"` };
  const result = await request(target, 'PUT', body, headers);
  return result.revision;
}

export async function pullProject(target) {
  const result = await request(target, 'GET');
  const text = new TextDecoder('utf-8', { fatal: true }).decode(result.bytes);
  const project = parseProject(text);
  if (project.version !== 2) throw new Error('현재 배치를 포함한 version 2 작업물이 필요합니다.');
  return { project, revision: result.revision };
}
