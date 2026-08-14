// Qt 6.10's qtprotobufgen emits this private include for every generated
// translation unit, even when the schema does not use well-known types.
// Some distributions do not install the corresponding private header.  The
// generated code does not reference anything from it for this schema, so an
// intentionally empty compatibility header keeps the public Qt Protobuf API
// usable without depending on Qt private headers.
