; C++-only tokens not covered by tree-sitter-c highlights.

[
 "catch"
 "class"
 "constexpr"
 "delete"
 "explicit"
 "friend"
 "mutable"
 "namespace"
 "new"
 "noexcept"
 "template"
 "throw"
 "try"
 "typename"
 "using"
 "virtual"
] @keyword

(call_expression
  function: (qualified_identifier
    name: (identifier) @function))

"nullptr" @constant
