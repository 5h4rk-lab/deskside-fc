import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { buildDetailFixture, buildMatchesFixture, fixtureDir } from "./build-fixtures.js";

const write = (name: string, value: unknown): void => {
  writeFileSync(join(fixtureDir, name), JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote contract/fixtures/${name}`);
};

write("matches-normalised.json", buildMatchesFixture());
write("detail-normalised.json", buildDetailFixture());
