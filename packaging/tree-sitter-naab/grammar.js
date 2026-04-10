// tree-sitter grammar for NAAb Block Assembly Language
// To use: create a tree-sitter-naab repo, place this file, run `tree-sitter generate`

module.exports = grammar({
  name: 'naab',

  extras: $ => [/\s/, $.comment],

  rules: {
    source_file: $ => repeat(choice(
      $.use_statement,
      $.import_statement,
      $.export_statement,
      $.function_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.main_block,
      $.runtime_declaration,
    )),

    comment: $ => choice(
      seq('//', /.*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    ),

    // Top-level constructs
    main_block: $ => seq('main', $.block),

    function_declaration: $ => seq(
      optional('async'),
      'function',
      $.identifier,
      $.parameter_list,
      optional(seq(':', $.type)),
      $.block,
    ),

    struct_declaration: $ => seq(
      'struct',
      $.identifier,
      optional(seq('implements', commaSep1($.identifier))),
      '{',
      repeat($.struct_field),
      '}',
    ),

    struct_field: $ => seq($.identifier, ':', $.type),

    enum_declaration: $ => seq(
      'enum',
      $.identifier,
      '{',
      commaSep1($.enum_variant),
      '}',
    ),

    enum_variant: $ => seq(
      $.identifier,
      optional(seq('(', commaSep($.type), ')')),
    ),

    use_statement: $ => seq('use', choice($.block_id, $.string, $.identifier), optional(seq('as', $.identifier))),
    import_statement: $ => seq('import', '{', commaSep1($.identifier), '}', 'from', $.string),
    export_statement: $ => seq('export', choice($.function_declaration, seq($.identifier))),
    runtime_declaration: $ => seq('runtime', $.identifier, '=', $.identifier, '.', 'start', '(', ')'),

    // Statements
    block: $ => seq('{', repeat($.statement), '}'),

    statement: $ => choice(
      $.var_declaration,
      $.if_statement,
      $.for_statement,
      $.while_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.try_statement,
      $.throw_statement,
      $.expression_statement,
    ),

    var_declaration: $ => seq(
      choice('let', 'const'),
      $.identifier,
      optional(seq(':', $.type)),
      optional(seq('=', $.expression)),
    ),

    if_statement: $ => seq(
      'if',
      optional('('), $.expression, optional(')'),
      $.block,
      optional(seq('else', choice($.if_statement, $.block))),
    ),

    for_statement: $ => seq(
      'for',
      optional('('),
      choice(
        seq($.identifier, 'in', $.expression),
        seq('(', $.identifier, ',', $.identifier, ')', 'in', $.expression),
        seq('[', commaSep1($.identifier), ']', 'in', $.expression),
      ),
      optional(')'),
      $.block,
    ),

    while_statement: $ => seq('while', optional('('), $.expression, optional(')'), $.block),
    return_statement: $ => seq('return', optional($.expression)),
    break_statement: $ => 'break',
    continue_statement: $ => 'continue',
    throw_statement: $ => seq('throw', $.expression),

    try_statement: $ => seq(
      'try', $.block,
      optional(seq('catch', '(', $.identifier, ')', $.block)),
      optional(seq('finally', $.block)),
    ),

    expression_statement: $ => $.expression,

    // Expressions (simplified — full grammar would need precedence climbing)
    expression: $ => choice(
      $.assignment,
      $.binary_expression,
      $.unary_expression,
      $.call_expression,
      $.member_expression,
      $.index_expression,
      $.match_expression,
      $.if_expression,
      $.lambda_expression,
      $.await_expression,
      $.primary,
    ),

    assignment: $ => prec.right(seq($.expression, choice('=', '+=', '-=', '*=', '/='), $.expression)),
    binary_expression: $ => choice(
      ...[['+', 10], ['-', 10], ['*', 20], ['/', 20], ['%', 20],
         ['==', 5], ['!=', 5], ['<', 5], ['>', 5], ['<=', 5], ['>=', 5],
         ['&&', 3], ['||', 2], ['??', 1], ['|>', 0], ['..', 4], ['..=', 4],
      ].map(([op, prec_val]) =>
        prec.left(prec_val, seq($.expression, op, $.expression))
      ),
    ),

    unary_expression: $ => choice(
      prec(30, seq('-', $.expression)),
      prec(30, seq('!', $.expression)),
      prec(30, seq('not', $.expression)),
    ),

    call_expression: $ => prec(40, seq($.expression, '(', commaSep($.expression), ')')),
    member_expression: $ => prec(40, seq($.expression, '.', $.identifier)),
    index_expression: $ => prec(40, seq($.expression, '[', $.expression, ']')),

    match_expression: $ => seq(
      choice('match', 'switch'),
      $.expression,
      '{', commaSep1($.match_arm), '}',
    ),

    match_arm: $ => seq(choice($.expression, '_'), '=>', $.expression),

    if_expression: $ => seq(
      'if', optional('('), $.expression, optional(')'),
      $.block,
      'else', choice($.if_expression, $.block),
    ),

    lambda_expression: $ => seq('fn', '(', commaSep($.identifier), ')', $.block),
    await_expression: $ => seq('await', $.expression),

    // Polyglot inline code
    inline_code: $ => seq(
      '<<', $.identifier, optional(seq('[', commaSep1($.identifier), ']')),
      /[^>]*/, // simplified — real grammar needs multiline support
      '>>',
    ),

    // Primary expressions
    primary: $ => choice(
      $.identifier,
      $.number,
      $.string,
      $.fstring,
      $.boolean,
      $.null,
      $.array_literal,
      $.dict_literal,
      seq('new', $.identifier, $.dict_literal),
      seq('(', $.expression, ')'),
    ),

    // Types
    type: $ => choice(
      'int', 'float', 'string', 'bool', 'any', 'void', 'null',
      seq($.identifier, optional(seq('<', commaSep1($.type), '>'))),
      seq('array', '<', $.type, '>'),
      seq('dict', '<', $.type, ',', $.type, '>'),
      seq('(', commaSep($.type), ')', '=>', $.type),
    ),

    parameter_list: $ => seq('(', commaSep($.parameter), ')'),
    parameter: $ => seq($.identifier, optional(seq(':', $.type)), optional(seq('=', $.expression))),

    // Literals
    identifier: $ => /[a-zA-Z_][a-zA-Z0-9_]*/,
    number: $ => choice(/\d+/, /\d+\.\d+/, /0x[0-9a-fA-F]+/, /0b[01]+/, /0o[0-7]+/),
    string: $ => choice(
      seq('"', repeat(choice(/[^"\\$]+/, /\\./, seq('${', /[^}]*/, '}'))), '"'),
      seq("'", repeat(choice(/[^'\\]+/, /\\./)), "'"),
    ),
    fstring: $ => seq('f', choice(
      seq('"', repeat(choice(/[^"\\{]+/, /\\./, seq('{', /[^}]*/, '}'))), '"'),
      seq("'", repeat(choice(/[^'\\{]+/, /\\./, seq('{', /[^}]*/, '}'))), "'"),
    )),
    boolean: $ => choice('true', 'false'),
    null: $ => 'null',
    array_literal: $ => seq('[', commaSep($.expression), ']'),
    dict_literal: $ => seq('{', commaSep(seq(choice($.identifier, $.string), ':', $.expression)), '}'),
    block_id: $ => /BLOCK-[A-Z]+-\d+/,
  },
});

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}
