// Pure's experimental scalar interaction language. No host code is evaluated.
const LIMITS = Object.freeze({ sourceBytes: 32768, states: 32, inputs: 8,
  views: 24, handlers: 64, instructions: 512, depth: 64, stack: 64 });
const RESERVED = new Set(['frame', 'dt', 'event_x', 'event_y', 'min', 'max', 'clamp']);
const BINARY = new Map([
  ['||', [1, 15]], ['&&', [2, 14]], ['==', [3, 12]], ['!=', [3, 13]],
  ['<', [4, 8]], ['<=', [4, 9]], ['>', [4, 10]], ['>=', [4, 11]],
  ['+', [5, 3]], ['-', [5, 4]], ['*', [6, 5]], ['/', [6, 6]],
]);
const FUNCTIONS = { min: [2, 17], max: [2, 18], clamp: [3, 19] };
const VIEW_FIELDS = { box: ['x', 'y', 'width', 'height'],
  circle: ['x', 'y', 'radius'], text: ['x', 'y', 'value'] };

function fail(token, message) {
  const error = new Error(`${message} (${token.line}:${token.column})`);
  error.line = token.line;
  error.column = token.column;
  throw error;
}

function lex(source) {
  const tokens = [];
  let offset = 0, line = 1, column = 1;
  const advance = () => {
    const char = source[offset++];
    if (char === '\n') { line++; column = 1; } else column++;
    return char;
  };
  while (offset < source.length) {
    const char = source[offset];
    if (char === ' ' || char === '\t' || char === '\r') { advance(); continue; }
    if (char === '#') { while (offset < source.length && source[offset] !== '\n') advance(); continue; }
    const token = { line, column };
    if (char === '\n') { advance(); tokens.push({ ...token, kind: 'nl' }); continue; }
    if (char === '"') {
      advance();
      let value = '', closed = false;
      while (offset < source.length) {
        const next = advance();
        if (next === '"') { closed = true; break; }
        if (next === '\n' || next === '\r') fail(token, 'Strings must fit on one line');
        if (next === '\\') {
          const escaped = advance();
          const escapes = { n: '\n', r: '\r', t: '\t', '"': '"', '\\': '\\' };
          if (!Object.hasOwn(escapes, escaped)) fail(token, 'Unsupported string escape');
          value += escapes[escaped];
        } else value += next;
      }
      if (!closed) fail(token, 'Unterminated string');
      tokens.push({ ...token, kind: 'string', value }); continue;
    }
    const remaining = source.slice(offset);
    const number = remaining.match(/^(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/);
    if (number) {
      const value = Number(number[0]);
      if (!Number.isFinite(value)) fail(token, 'Number must be finite');
      for (let i = 0; i < number[0].length; i++) advance();
      tokens.push({ ...token, kind: 'number', value }); continue;
    }
    const name = remaining.match(/^[A-Za-z_][A-Za-z_0-9]*/);
    if (name) {
      for (let i = 0; i < name[0].length; i++) advance();
      tokens.push({ ...token, kind: 'name', value: name[0] }); continue;
    }
    const pair = source.slice(offset, offset + 2);
    if (['<=', '>=', '==', '!=', '&&', '||'].includes(pair)) {
      advance(); advance(); tokens.push({ ...token, kind: pair }); continue;
    }
    if ('=+-*/!<>(){},.;:'.includes(char)) {
      advance(); tokens.push({ ...token, kind: char }); continue;
    }
    fail(token, `Unexpected character ${JSON.stringify(char)}`);
  }
  tokens.push({ kind: 'eof', line, column });
  return tokens;
}

class Parser {
  constructor(tokens) { this.tokens = tokens; this.position = 0; this.depth = 0; }
  peek() { return this.tokens[this.position]; }
  take() { return this.tokens[this.position++]; }
  is(kind, value) { return this.peek().kind === kind && (value === undefined || this.peek().value === value); }
  expect(kind, description = kind) {
    if (!this.is(kind)) fail(this.peek(), `Expected ${description}`);
    return this.take();
  }
  newlines() { while (this.is('nl')) this.take(); }
  separators() { while (this.is('nl') || this.is(';')) this.take(); }
  end() {
    if (!['nl', ';', '}', 'eof'].includes(this.peek().kind)) fail(this.peek(), 'Expected newline or semicolon');
    this.separators();
  }
  reference(token) {
    if (!this.is('.')) return token.value;
    this.take();
    return `${token.value}.${this.expect('name', 'field name').value}`;
  }
  initial() {
    let sign = 1;
    if (this.is('-') || this.is('+')) sign = this.take().kind === '-' ? -1 : 1;
    return sign * this.expect('number', 'numeric state initializer').value;
  }
  node(token, properties, children = []) {
    const depth = 1 + Math.max(0, ...children.map(child => child.depth));
    if (depth > LIMITS.depth) fail(token, 'Expression nesting exceeds 64');
    return { token, depth, ...properties };
  }
  expression(minimum = 0, multiline = false) {
    if (++this.depth > LIMITS.depth) fail(this.peek(), 'Expression nesting exceeds 64');
    if (multiline) this.newlines();
    const token = this.take();
    let left;
    if (token.kind === 'number') left = this.node(token, { kind: 'constant', value: token.value });
    else if (token.kind === '-' || token.kind === '!') {
      const child = this.expression(7, multiline);
      left = this.node(token, { kind: 'unary', op: token.kind === '-' ? 7 : 16, child }, [child]);
    } else if (token.kind === '(') {
      left = this.expression(0, true); this.newlines(); this.expect(')');
    } else if (token.kind === 'name') {
      if (Object.hasOwn(FUNCTIONS, token.value) && this.is('(')) {
        this.take();
        const args = [];
        this.newlines();
        if (!this.is(')')) {
          for (;;) {
            args.push(this.expression(0, true)); this.newlines();
            if (!this.is(',')) break;
            this.take();
          }
        }
        this.expect(')');
        if (args.length !== FUNCTIONS[token.value][0]) fail(token, `${token.value} expects ${FUNCTIONS[token.value][0]} arguments`);
        left = this.node(token, { kind: 'call', op: FUNCTIONS[token.value][1], args }, args);
      } else left = this.node(token, { kind: 'reference', name: this.reference(token) });
    } else fail(token, 'Expected a number, state, or expression');
    for (;;) {
      if (multiline) this.newlines();
      const entry = BINARY.get(this.peek().kind);
      if (!entry || entry[0] < minimum) break;
      const operator = this.take();
      const right = this.expression(entry[0] + 1, multiline);
      left = this.node(operator, { kind: 'binary', op: entry[1], left, right }, [left, right]);
    }
    this.depth--;
    return left;
  }
  block(depth = 0) {
    if (depth >= LIMITS.depth) fail(this.peek(), 'Handler nesting exceeds 64');
    this.newlines(); this.expect('{'); this.separators();
    const statements = [];
    while (!this.is('}')) {
      const token = this.expect('name', 'assignment, if, or closing brace');
      if (token.value === 'if' && !this.is('=')) {
        const condition = this.expression();
        const body = this.block(depth + 1);
        statements.push({ kind: 'if', token, condition, body });
      } else {
        const name = this.reference(token);
        this.expect('=');
        statements.push({ kind: 'assign', token, name, expression: this.expression() });
      }
      this.end();
    }
    this.take();
    return statements;
  }
  parse() {
    const declarations = [];
    this.separators();
    while (!this.is('eof')) {
      const token = this.expect('name', 'declaration');
      const kind = token.value;
      if (kind === 'state') {
        const name = this.expect('name', 'state name'); this.expect('=');
        declarations.push({ kind, token, name, initial: this.initial() });
      } else if (kind === 'record') {
        const name = this.expect('name', 'record type'); this.newlines(); this.expect('{'); this.separators();
        const fields = [];
        while (!this.is('}')) {
          const field = this.expect('name', 'field name'); this.expect('=');
          fields.push({name:field, initial:this.initial()}); this.end();
        }
        this.take(); declarations.push({kind,token,name,fields});
      } else if (kind === 'item') {
        const name = this.expect('name', 'item name'); this.expect('=');
        const type = this.expect('name', 'record type'); this.expect('('); this.newlines();
        const fields = [];
        while (!this.is(')')) {
          const field = this.expect('name', 'field name'); this.expect('=');
          fields.push({name:field, initial:this.initial()}); this.newlines();
          if (!this.is(',')) break;
          this.take(); this.newlines();
        }
        this.expect(')'); declarations.push({kind,token,name,type,fields});
      } else if (kind === 'rule') {
        const name = this.expect('name', 'rule name'); this.expect('(');
        const parameter = this.expect('name', 'record parameter'); this.expect(':');
        const type = this.expect('name', 'record type'); this.expect(')');
        declarations.push({kind,token,name,parameter,type,body:this.block()});
      } else if (kind === 'input') {
        const name = this.expect('name', 'input name'); this.expect('=');
        const key = this.expect('name', 'keyboard code or pointer(view)');
        if (key.value === 'pointer') {
          this.expect('(');
          const target = this.expect('name', 'view ID'); this.expect(')');
          declarations.push({ kind, token, name, target });
          this.end(); continue;
        }
        if (!/^(?:Space|Enter|Arrow(?:Left|Right|Up|Down)|Key[A-Z]|Digit[0-9])$/.test(key.value)) fail(key, 'Unsupported keyboard code');
        declarations.push({ kind, token, name, key });
      } else if (Object.hasOwn(VIEW_FIELDS, kind)) {
        const name = this.expect('name', 'view ID'); this.expect('(');
        const expressions = [];
        for (let i = 0; i < VIEW_FIELDS[kind].length; i++) {
          if (i) { this.newlines(); this.expect(','); }
          expressions.push(this.expression(0, true));
        }
        this.newlines();
        let color, prefix = '';
        if (kind !== 'text') {
          this.expect(','); this.newlines();
          const value = this.expect('string', 'literal hex color');
          if (!/^#[0-9a-fA-F]{6}$/.test(value.value)) fail(value, 'Color must use six hexadecimal digits, such as "#356548"');
          color = value.value;
        } else if (this.is(',')) { this.take(); this.newlines(); prefix = this.expect('string', 'literal text prefix').value; }
        this.newlines(); this.expect(')');
        declarations.push({ kind: 'view', type: kind, token, name, expressions, color, prefix });
      } else if (kind === 'on') {
        const event = this.expect('name', 'input name or frame');
        let edge;
        if (event.value !== 'frame') {
          this.expect('.'); edge = this.expect('name', 'down, up, move or cancel');
          if (!['down', 'up', 'move', 'cancel'].includes(edge.value)) fail(edge, 'Expected down or up, move or cancel');
        }
        if (this.is('name', 'use')) {
          this.take(); const rule = this.expect('name', 'rule name'); this.expect('(');
          const item = this.expect('name', 'item name'); this.expect(')');
          declarations.push({kind:'handler',token,event,edge,rule,item});
        } else declarations.push({ kind: 'handler', token, event, edge, body: this.block() });
      } else fail(token, `Unknown declaration ${kind}`);
      this.end();
    }
    return declarations;
  }
}

export function compile(source) {
  if (typeof source !== 'string') fail({ line: 1, column: 1 }, 'Source must be a string');
  const sourceBytes = new TextEncoder().encode(source).length;
  if (sourceBytes > LIMITS.sourceBytes) fail({ line: 1, column: 1 }, 'Source exceeds 32768 UTF-8 bytes');
  const declarations = new Parser(lex(source)).parse();
  const program = { states: [], inputs: [], instructions: [], handlers: [], views: [], metrics: {} };
  const states = new Map(), inputs = new Map(), views = new Set(), keys = new Set(), targets = new Set();
  const records = new Map(), items = new Map(), rules = new Map();
  const scalarNames = new Set(declarations.filter(d => d.kind === 'state').map(d => d.name.value));
  for (const declaration of declarations) {
    if (!['record','rule'].includes(declaration.kind)) continue;
    const {name,fields,kind} = declaration, symbols = kind === 'record' ? records : rules;
    if (RESERVED.has(name.value)) fail(name, `Reserved name ${name.value}`);
    if (symbols.has(name.value)) fail(name, `Duplicate ${kind} ${name.value}`);
    if (kind === 'record') {
      if (!fields.length || fields.length > 8) fail(name, 'A record needs 1..8 fields');
      const used = new Set();
      for (const field of fields) {
        if (used.has(field.name.value)) fail(field.name, `Duplicate field ${field.name.value}`);
        if (RESERVED.has(field.name.value)) fail(field.name, `Reserved name ${field.name.value}`);
        used.add(field.name.value);
      }
    }
    symbols.set(name.value, declaration);
  }
  let handlerCount = 0;
  for (const declaration of declarations) {
    const { kind, name, token } = declaration;
    if (name && RESERVED.has(name.value)) fail(name, `Reserved name ${name.value}`);
    if (kind === 'record' || kind === 'rule') continue;
    if (kind === 'item') {
      if (items.has(name.value) || scalarNames.has(name.value)) fail(name, `Duplicate state/item ${name.value}`);
      const record = records.get(declaration.type.value);
      if (!record) fail(declaration.type, `Unknown record ${declaration.type.value}`);
      const overrides = new Map();
      for (const field of declaration.fields) {
        if (!record.fields.some(f => f.name.value === field.name.value)) fail(field.name, `Unknown field ${field.name.value}`);
        if (overrides.has(field.name.value)) fail(field.name, `Duplicate field ${field.name.value}`);
        overrides.set(field.name.value, field.initial);
      }
      const base = states.size;
      for (const field of record.fields) {
        if (states.size >= LIMITS.states) fail(name, 'At most 32 states are allowed, including item fields');
        const qualified = `${name.value}.${field.name.value}`, index = states.size;
        states.set(qualified,index);
        program.states.push({name:qualified,initial:overrides.has(field.name.value) ? overrides.get(field.name.value) : field.initial,line:token.line});
      }
      items.set(name.value,{type:declaration.type.value,base,count:record.fields.length});
    } else if (kind === 'state' || kind === 'input') {
      const symbols = kind === 'state' ? states : inputs;
      if (symbols.has(name.value)) fail(name, `Duplicate ${kind} ${name.value}`);
      if (symbols.size >= LIMITS[`${kind}s`]) fail(name, `At most ${LIMITS[`${kind}s`]} ${kind}s are allowed`);
      symbols.set(name.value, symbols.size);
      if (kind === 'state') program.states.push({ name: name.value, initial: declaration.initial, line: token.line });
      else if (declaration.target) {
        const target = declaration.target;
        if (targets.has(target.value)) fail(target, `Duplicate pointer target ${target.value}`);
        targets.add(target.value);
        program.inputs.push({ name: name.value, target: target.value, line: token.line });
      } else {
        if (keys.has(declaration.key.value)) fail(declaration.key, `Duplicate keyboard code ${declaration.key.value}`);
        keys.add(declaration.key.value);
        program.inputs.push({ name: name.value, key: declaration.key.value, line: token.line });
      }
    } else if (kind === 'view') {
      if (views.has(name.value)) fail(name, `Duplicate view ID ${name.value}`);
      if (views.size >= LIMITS.views) fail(name, 'At most 24 views are allowed');
      views.add(name.value);
    } else if (++handlerCount > LIMITS.handlers) fail(token, 'At most 64 handlers are allowed');
  }
  for (const declaration of declarations) {
    if (declaration.target && !views.has(declaration.target.value))
      fail(declaration.target, `Unknown pointer target ${declaration.target.value}`);
  }
  for (const rule of rules.values()) {
    if (!records.has(rule.type.value)) fail(rule.type, `Unknown record ${rule.type.value}`);
    if (RESERVED.has(rule.parameter.value)) fail(rule.parameter, `Reserved name ${rule.parameter.value}`);
  }
  let pointHandler = false;
  let parameters = null;
  let parameterPrefix = null;
  let stack = 0, maxStack = 0;
  const emit = (op, token, arg = 0, value = 0, delta = 0) => {
    if (program.instructions.length >= LIMITS.instructions) fail(token, 'At most 512 instructions are allowed');
    stack += delta;
    if (stack < 0 || stack > LIMITS.stack) fail(token, 'Expression stack exceeds 64');
    maxStack = Math.max(maxStack, stack);
    program.instructions.push({ op, arg, value, line: token.line });
  };
  const expression = (node, binding = false) => {
    const { token } = node;
    if (node.kind === 'constant') emit(0, token, 0, node.value, 1);
    else if (node.kind === 'reference') {
      if (node.name === 'event_x' || node.name === 'event_y') {
        if (binding || !pointHandler) fail(token, 'event_x and event_y are only available in pointer handlers');
        emit(node.name === 'event_x' ? 22 : 23, token, 0, 0, 1);
      } else if (node.name === 'dt') {
        if (binding) fail(token, 'dt is only available in handlers');
        emit(2, token, 0, 0, 1);
      } else {
        if (parameterPrefix && node.name.startsWith(parameterPrefix) && !parameters.has(node.name))
          fail(token, `Unknown parameter field ${node.name}`);
        if (parameters?.has(node.name)) {
          emit(24, token, parameters.get(node.name), 0, 1);
          return;
        }
        if (!states.has(node.name)) fail(token, `Unknown state ${node.name}`);
        emit(1, token, states.get(node.name), 0, 1);
      }
    } else if (node.kind === 'unary') { expression(node.child, binding); emit(node.op, token); }
    else if (node.kind === 'binary') {
      expression(node.left, binding); expression(node.right, binding); emit(node.op, token, 0, 0, -1);
    } else {
      for (const arg of node.args) expression(arg, binding);
      emit(node.op, token, 0, 0, 1 - node.args.length);
    }
  };
  const statements = body => {
    for (const statement of body) {
      if (statement.kind === 'assign') {
        if (parameterPrefix && statement.name.startsWith(parameterPrefix) && !parameters.has(statement.name))
          fail(statement.token, `Unknown parameter field ${statement.name}`);
        const parameter = parameters?.get(statement.name);
        if (parameter === undefined && !states.has(statement.name)) fail(statement.token, `Unknown state ${statement.name}`);
        expression(statement.expression);
        emit(parameter === undefined ? 20 : 25, statement.token, parameter === undefined ? states.get(statement.name) : parameter, 0, -1);
      } else {
        expression(statement.condition);
        const branch = program.instructions.length;
        emit(21, statement.token, 0, 0, -1);
        statements(statement.body);
        program.instructions[branch].arg = program.instructions.length;
      }
    }
  };
  const bodies = new Map();
  function ruleBody(rule) {
    if (bodies.has(rule.name.value)) return bodies.get(rule.name.value);
    parameterPrefix = `${rule.parameter.value}.`;
    parameters = new Map(records.get(rule.type.value).fields.map((field,index) => [`${rule.parameter.value}.${field.name.value}`,index]));
    // A shared body may read coordinates; every binding is checked below.
    pointHandler = true; stack = 0;
    const start = program.instructions.length;
    statements(rule.body);
    const length = program.instructions.length - start;
    const point = program.instructions.slice(start).some(op => op.op === 22 || op.op === 23);
    parameters = null; parameterPrefix = null;
    const body = {start,length,point}; bodies.set(rule.name.value,body); return body;
  }
  for (const declaration of declarations) {
    if (declaration.kind === 'handler') {
      let event = 0;
      pointHandler = false;
      if (declaration.event.value !== 'frame') {
        if (!inputs.has(declaration.event.value)) fail(declaration.event, `Unknown input ${declaration.event.value}`);
        const index = inputs.get(declaration.event.value), edge = declaration.edge.value;
        pointHandler = Boolean(program.inputs[index].target);
        if (!pointHandler && (edge === 'move' || edge === 'cancel')) fail(declaration.edge, 'move and cancel require a pointer input');
        event = edge === 'move' ? 17 + index : edge === 'cancel' ? 25 + index : 1 + index * 2 + (edge === 'up' ? 1 : 0);
      }
      if (declaration.rule) {
        const rule = rules.get(declaration.rule.value), item = items.get(declaration.item.value);
        if (!rule) fail(declaration.rule, `Unknown rule ${declaration.rule.value}`);
        if (!item) fail(declaration.item, `Unknown item ${declaration.item.value}`);
        if (item.type !== rule.type.value) fail(declaration.item, `Rule ${rule.name.value} requires ${rule.type.value}`);
        const pointer = pointHandler;
        const body = ruleBody(rule);
        if (body.point && !pointer) fail(declaration.rule, 'Coordinate-reading rules require a pointer input');
        program.handlers.push({event,start:body.start,length:body.length,line:declaration.token.line,parameterBase:item.base,parameterCount:item.count});
      } else {
        const start = program.instructions.length;
        stack = 0; statements(declaration.body);
        program.handlers.push({ event, start, length: program.instructions.length - start, line: declaration.token.line });
      }
    } else if (declaration.kind === 'view') {
      const view = { id: declaration.name.value, type: declaration.type, line: declaration.token.line, bindings: {} };
      if (view.type === 'text') view.prefix = declaration.prefix;
      else view.color = declaration.color;
      declaration.expressions.forEach((node, index) => {
        const start = program.instructions.length;
        stack = 0; expression(node, true);
        view.bindings[VIEW_FIELDS[view.type][index]] = { start, length: program.instructions.length - start };
      });
      program.views.push(view);
    }
  }
  // Unused definitions still receive name/type checking, but reserve no VM body.
  for (const rule of rules.values()) {
    if (bodies.has(rule.name.value)) continue;
    const instructions = program.instructions, previousMax = maxStack;
    program.instructions = [];
    ruleBody(rule);
    program.instructions = instructions; maxStack = previousMax;
  }
  program.metrics = { sourceBytes, sourceLines: source === '' ? 0 : source.split('\n').length,
    states: states.size, inputs: inputs.size, views: views.size,
    handlers: program.handlers.length, instructions: program.instructions.length, maxStack };
  return program;
}

export { LIMITS };
