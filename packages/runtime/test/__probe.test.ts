import { describe, expect, it } from 'vitest';

describe('probe', () => {
	it('imports fetch.ts with a stubbed $', async () => {
		const calls: string[] = [];
		const magic: any = new Proxy(function () {}, {
			get(_t: any, prop: string) {
				if (prop === Symbol.toPrimitive) return () => 0;
				if (prop === 'toString') return () => '';
				if (prop === Symbol.iterator) return function* () {};
				return magic;
			},
			apply() {
				return magic;
			},
			construct() {
				return magic;
			},
		});
		(globalThis as any).$ = new Proxy(
			{ tcpRxBufSize: 262144 },
			{
				get(target: any, prop: string) {
					if (prop in target) return target[prop];
					return (...args: unknown[]) => {
						calls.push(prop);
						return magic;
					};
				},
			},
		);
		const mod = await import('../src/fetch/fetch');
		expect(typeof mod.fetch).toBe('function');
		console.error('module-level $ calls:', JSON.stringify(calls));
	});
});
