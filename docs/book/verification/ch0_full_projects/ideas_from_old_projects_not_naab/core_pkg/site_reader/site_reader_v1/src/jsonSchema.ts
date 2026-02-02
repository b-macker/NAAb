import Ajv from 'ajv/dist/jtd';

const ajv = new Ajv();

export class SchemaWhisperer {
  static inferSchema(jsonStr: string) {
    try {
      const data = JSON.parse(jsonStr);
      // Use ajv's private API to derive JTD schema heuristically
      // @ts-ignore
      const serializer = ajv.compileSerializer(data);
      return serializer.schema;
    } catch {
      return null;
    }
  }
}
