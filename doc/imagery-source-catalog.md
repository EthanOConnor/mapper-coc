# Imagery source catalogs: design and implementation specification

Status: draft for review

This document specifies a portable catalog of online imagery sources for
OpenOrienteering Mapper. It covers the interchange format, import and
installation behavior, validation, source identity, custom tile grids, and
future surveyed registration corrections.

The first implementation is intended for the COC Mapper preview, but the
format and implementation are deliberately OpenOrienteering-neutral. Nothing
in the core format is tied to COC, a particular imagery provider, or a
particular jurisdiction.

## 1. Problem statement

Mapper can create a tiled online template from an XYZ URL or ArcGIS cached
MapServer URL. The initial implementation is useful for one-off sources, but
clubs and individual mappers also need to maintain and exchange curated sets
of sources without copying URLs and expert georeferencing values by hand.

An imagery source catalog must:

- remain compact and reasonably human-readable and editable;
- install from a local file or an HTTP(S) URL;
- persist for the current Mapper installation;
- describe ordinary global Web Mercator sources and nonstandard tile grids;
- carry attribution and source-supplied terms without deciding whether a user
  is legally permitted to use the imagery;
- detect duplicates and catalog revisions predictably;
- represent source-specific surveyed offsets, affine corrections, and grid
  shifts without silently discarding unsupported corrections; and
- avoid broad changes to Mapper's existing template and GDAL architecture.

## 2. Scope

### 2.1 First implementation

The first implementation includes:

- a versioned JSON catalog format and published JSON Schema;
- strict parsing and semantic validation;
- standard and inline OGC tile matrix sets;
- local-file and HTTP(S) catalog import;
- an import preview, duplicate/conflict reporting, and large-catalog warning;
- per-user persistent catalog snapshots and catalog removal;
- catalog entries in the existing online imagery source chooser;
- generic source-CRS tile-grid cropping, including the Pierce County
  EPSG:2927 grid used by the test fixture;
- runtime support for two-dimensional translation corrections;
- explicit representation and safe capability handling for affine and grid
  corrections; and
- offline automated tests plus a manual Windows installer smoke test.

### 2.2 Deferred work

The following are intentionally not part of the first implementation:

- a graphical catalog or source editor;
- automatic catalog refresh;
- recursive catalog includes;
- catalog signing or a trust/reputation system;
- arbitrary HTTP authentication or secrets in catalogs;
- arbitrary GDAL XML or PROJ pipelines supplied by a catalog;
- general WMS, WMTS, OGC API Tiles, or STAC service discovery; and
- execution of affine or grid-shift registrations until the required
  GDAL VRT/warp path is implemented and tested.

The schema defines affine and grid-shift operations now so that catalog
authors do not need a later incompatible format. A source requiring an
operation unsupported by the running Mapper build is retained but disabled.
It must never be rendered using its uncorrected nominal georeferencing.

## 3. Terminology

**Catalog**
: A versioned JSON document containing one or more imagery source
  definitions and catalog-level metadata.

**Source**
: One selectable imagery service definition. A source includes access,
  tiling, notice, coverage, and optional registration information.

**Tile matrix set**
: The server's coordinate reference system, tile origin, scale levels,
  dimensions, and row/column indexing. This describes how to request and
  georeference the source's tiles.

**Registration**
: A correction mapping coordinates in the source's nominal frame to a
  corrected or locally surveyed frame. Registration is separate from the tile
  matrix set.

**Operational fingerprint**
: A digest of the fields that determine requests and rendered
  georeferencing. It is used to detect equivalent or conflicting source
  definitions.

## 4. Existing implementation constraints

`OnlineImagerySource` currently contains only source kind, display name,
normalized URL, tile size, and maximum tile level. The template builder then
converts the selected map extent to Web Mercator, snaps it to the global
Web Mercator tile grid, and writes `EPSG:3857` into a GDAL WMS XML file.

Catalog support cannot be implemented as a list of saved URLs. The runtime
source model must also carry:

- a CRS;
- a standard or inline tile matrix set;
- usable matrix limits;
- request behavior such as a referer and empty-tile status codes; and
- an optional registration correction.

Manual URL classification remains supported. It produces the same richer
runtime source definition using the standard WebMercatorQuad matrix set and
the current conservative defaults.

## 5. Standards and format decision

No single existing interchange format covers the complete requirement.

- The OSM Editor Layer Index is the closest catalog precedent, but it is
  designed around OpenStreetMap editors and contains OSM-specific permission
  semantics.
- TileJSON supplies useful vocabulary for tile URL templates, scheme, bounds,
  zoom limits, names, and attribution, but describes one tileset and assumes
  the global Mercator profile for `xyz` and `tms` schemes.
- OGC Two Dimensional Tile Matrix Set and Tile Set Metadata 2.0 defines the
  required grid model for standard and arbitrary CRSs, origins, scale levels,
  tile sizes, and matrix dimensions.
- STAC is aimed at discovery and description of geospatial assets, not at
  installing desktop-client tile service configuration.

The catalog therefore uses a small OpenOrienteering JSON wrapper, familiar
TileJSON and Editor Layer Index concepts where appropriate, and OGC 2D Tile
Matrix Set objects for grid definitions.

The initial filename suffix is `.oom-imagery.json`. The proposed media type is
`application/vnd.openorienteering.imagery-catalog+json`.

The term **package** is reserved for a future archive containing the same
catalog manifest and referenced binary resources such as displacement grids.
The first implementation imports the JSON catalog only.

References:

- <https://github.com/osmlab/editor-layer-index>
- <https://github.com/mapbox/tilejson-spec/tree/master/3.0.0>
- <https://www.ogc.org/standards/tms/>
- <https://schemas.opengis.net/tms/2.0/json/>
- <https://github.com/radiantearth/stac-spec>

## 6. Catalog document

Catalog files are UTF-8 JSON. The normative JSON Schema uses
`additionalProperties: false` for defined objects. Future experimental data
belongs under a namespaced `extensions` object rather than as arbitrary
top-level members.

An illustrative catalog is:

```json
{
  "$schema": "https://openorienteering.org/schemas/imagery-catalog/v1.json",
  "format": "org.openorienteering.imagery-catalog",
  "version": 1,
  "id": "org.cascadeoc.imagery.puget-sound-example",
  "revision": 1,
  "name": "Puget Sound imagery examples",
  "description": "Example definitions for catalog and grid testing.",
  "publisher": {
    "name": "Cascade Orienteering Club",
    "url": "https://cascadeoc.org/"
  },
  "updated": "2026-07-09",
  "sources": [
    {
      "id": "wa.king.aerial-2025",
      "name": "King County Aerial 2025",
      "type": "raster-tiles",
      "tiles": [
        "https://gismaps.kingcounty.gov/arcgis/rest/services/BaseMaps/KingCo_Aerial_2025/MapServer/tile/{z}/{y}/{x}"
      ],
      "scheme": "xyz",
      "tileMatrixSetURI": "http://www.opengis.net/def/tilematrixset/OGC/1.0/WebMercatorQuad",
      "minTileMatrix": "0",
      "maxTileMatrix": "20",
      "request": {
        "emptyHttpStatusCodes": [404]
      },
      "notices": {
        "sourceUrl": "https://gismaps.kingcounty.gov/arcgis/rest/services/BaseMaps/KingCo_Aerial_2025/MapServer"
      }
    }
  ]
}
```

The example is deliberately a source definition, not a statement that the
imagery is approved for every use or should be bundled in the product.

### 6.1 Required catalog members

`format`
: Must equal `org.openorienteering.imagery-catalog`.

`version`
: Integer format version. Version 1 parsers reject a higher version rather
  than guessing how new required semantics should behave.

`id`
: Stable publisher-controlled identifier. Reverse-DNS style is recommended.
  It is the identity used for catalog updates.

`revision`
: Positive monotonically increasing integer. Changing catalog content should
  increment it.

`name`
: Plain-text user-facing catalog name.

`sources`
: Nonempty array of source definitions. Source IDs must be unique within a
  catalog.

### 6.2 Optional catalog members

- `description`: plain-text description;
- `publisher`: plain-text name plus optional URL and contact URL;
- `created` and `updated`: ISO 8601 dates;
- `catalogLicense`: license applying to the catalog metadata itself, not to
  the imagery reached through it;
- `requires`: required application capabilities;
- `resources`: checksummed external or packaged resources reserved for future
  registration support; and
- `extensions`: reverse-DNS-namespaced extension data.

## 7. Source definitions

### 7.1 Identity and presentation

Every source has a stable `id`, `name`, and `type`. Version 1 supports
`raster-tiles`.

Optional presentation members include:

- `description`;
- `startDate` and `endDate` for imagery acquisition date or range;
- `category`, initially `aerial`, `satellite`, `map`, `elevation`, or
  `other`; and
- `coverage`, a WGS84 GeoJSON geometry used for display and future filtering.

Coverage is descriptive and must not be used in place of tile matrix limits.
Some ArcGIS services advertise a cache extent much larger than their useful
imagery coverage.

All user-facing strings are plain text. HTML supplied by a catalog is never
rendered.

### 7.2 Tile access

`tiles` is a nonempty array of absolute URL templates. Equivalent endpoints
may be listed for load distribution, but each must return the same content for
the same logical tile.

Version 1 recognizes `{z}`, `{x}`, and `{y}`. It also accepts Mapper's current
`${z}`, `${x}`, and `${y}` input spelling and canonicalizes it for comparison.
The stored URL is not rewritten, which avoids changing signed or
order-sensitive query strings.

`scheme` is `xyz` or `tms` and controls row-number interpretation. Tile matrix
geometry remains authoritative.

Optional access members include:

- `format`, an image media type;
- `minTileMatrix` and `maxTileMatrix`, identifiers ordered by the referenced
  tile matrix set;
- `tileMatrixLimits`, following the OGC limits model when availability does
  not fill complete matrices; and
- `request`, containing request behavior accepted by Mapper.

Version 1 request behavior is intentionally narrow:

- `referer`: an absolute HTTP(S) URL;
- `emptyHttpStatusCodes`: a unique array of integer HTTP status codes; and
- `emptyTileChecksums`: optional SHA-256 digests for known placeholder tiles.

Catalogs cannot provide `Authorization`, `Cookie`, `Proxy-Authorization`,
`Host`, or arbitrary custom headers. URLs containing user information are
invalid. A future credential-reference mechanism may associate a source with
installation-local secrets without putting those secrets in a shareable
catalog.

### 7.3 Tile matrix sets

A source contains exactly one of:

- `tileMatrixSetURI`, referencing a known OGC tile matrix set; or
- `tileMatrixSet`, containing an inline OGC 2D Tile Matrix Set 2.0 JSON object.

Mapper initially guarantees the registered WebMercatorQuad definition and
inline definitions. Referencing an unknown URI is an unsupported capability,
not permission to assume Web Mercator.

Inline validation checks:

- a parseable CRS;
- unique matrix identifiers;
- finite positive cell sizes;
- finite origins;
- positive tile and matrix dimensions;
- consistent axis and origin semantics;
- limits within the corresponding matrix dimensions; and
- an unambiguous ordering for `minTileMatrix` and `maxTileMatrix`.

The Pierce County test source uses an inline EPSG:2927 matrix set with origin
`890000, 967000`, 256-by-256 tiles, and the service's explicit levels. It must
not be represented as a Web Mercator source with an offset.

### 7.4 Notices and legal boundary

`notices` may contain:

- `attributionText`;
- `attributionUrl`;
- `sourceUrl`;
- `termsUrl`;
- `privacyUrl`; and
- `notes`.

These values are publisher-supplied information. Mapper may display them but
does not infer, certify, or enforce a permitted use. In particular, the
catalog schema does not contain an `allowed`, `publicDomain`, or similar
application-generated legal verdict.

The decision to include a source in Mapper's bundled catalog is a separate
product review. Test fixtures and example catalogs are not automatically
bundled catalogs.

## 8. Registration corrections

Registration maps nominal source coordinates into corrected coordinates.
Direction is never implicit.

```json
{
  "registration": {
    "direction": "source-to-corrected",
    "sourceFrame": {
      "crs": "EPSG:2927"
    },
    "targetFrame": {
      "crs": "EPSG:2927",
      "id": "org.example.club.survey-frame-2026"
    },
    "operation": {
      "type": "translation2d",
      "unit": "crs",
      "dx": -0.42,
      "dy": 0.17
    },
    "provenance": {
      "method": "survey-control",
      "observed": "2026-06-20",
      "author": "Example Mapping Team",
      "rmsError": 0.12,
      "notes": "Fit from six control points."
    }
  }
}
```

`sourceFrame` must agree with the tile matrix set CRS unless the operation
explicitly includes a CRS conversion supported by a later capability.
`targetFrame.id` allows a club to name an internal frame while still
specifying a conventional CRS where possible.

### 8.1 Translation

`translation2d` contains finite `dx` and `dy` values and an explicit unit.
`unit: crs` means the source/target CRS linear unit. Translation is reversible
and can be implemented in the first release by shifting the generated GDAL
data window.

### 8.2 Affine correction

`affine2d` uses named parameters matching the common affine equation:

```text
x' = xoff + s11*x + s12*y
y' = yoff + s21*x + s22*y
```

The determinant must be finite and nonzero. This aligns with the parameters
used by PROJ's affine operation and EPSG affine/similarity methods while
remaining readable.

The first release parses, validates, stores, and capability-gates this
operation. Rendering it requires a GDAL VRT wrapper with an arbitrary affine
geotransform and is deferred.

### 8.3 Grid shift

`gridShift` references an entry in the catalog's `resources` object:

```json
{
  "resources": {
    "local-horizontal-correction": {
      "href": "resources/local-horizontal-correction.tif",
      "mediaType": "image/tiff; application=geodetic-grid",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "size": 123456
    }
  }
}
```

The operation states the resource ID, horizontal/vertical domain, grid
reference frame, and interpolation method. GeoTIFF geodetic grids and NTv2
are the intended initial binary formats because they are supported by
PROJ/GDAL.

Relative resources are valid only inside a future catalog package or relative
to an explicitly trusted local catalog directory. Remote resources require
HTTPS, a declared size, and SHA-256 verification before use. The JSON-only
first implementation marks grid-shift sources unsupported and does not fetch
their resources.

References:

- <https://proj.org/en/stable/operations/transformations/affine.html>
- <https://proj.org/en/stable/operations/transformations/gridshift.html>
- <https://proj.org/en/stable/operations/transformations/hgridshift.html>

## 9. Versioning, required capabilities, and extensions

Format version protects document-level meaning. Capabilities protect
individual features that may be implemented at different times.

Initial capability identifiers are:

- `tile-matrix-set.ogc-2.0`;
- `registration.translation2d.v1`;
- `registration.affine2d.v1`; and
- `registration.grid-shift.v1`.

Unknown catalog-level required capabilities abort import. Unknown or
unsupported source-level required capabilities retain the catalog but disable
that source with a clear explanation.

Registration operations are inherently required for any source that declares
them. A catalog author cannot mark a registration correction optional.

Extensions live under reverse-DNS keys:

```json
{
  "extensions": {
    "org.example.mapping-team": {
      "reviewedBy": "Mapping Committee"
    }
  }
}
```

Mapper preserves the installed source document verbatim. It does not rewrite
unknown extension data.

## 10. Import user experience

The source chooser begins with:

```text
Choose a preset or recent source
Load source catalog...
Installed source catalogs...
--------------------------------
Bundled sources
  ...
Installed catalog name
  ...
--------------------------------
Recent manual sources
  ...
```

Section labels are disabled combo-box rows. Catalog sources store a stable
catalog/source handle in item data, not merely the displayed URL.

### 10.1 Loading

`Load source catalog...` opens a small dialog with:

- a file/URL field;
- a Browse button;
- progress and transport errors; and
- a Continue button enabled after the catalog document is available.

Local files are selected through the normal file picker. URL input accepts
HTTP and HTTPS only. HTTP is allowed after a warning because some clubs may
operate local services, but HTTPS is the default and expectation.

The importer does not fetch source tiles or service metadata. A catalog is
authoritative, and probing every source would create an unexpected network
storm and make import dependent on service availability.

### 10.2 Preview and confirmation

Every import displays a summary before installation:

- catalog name, publisher, origin, revision, and document hash;
- source names and distinct request hostnames;
- counts of new, exact duplicate, potential duplicate, changed, conflicting,
  invalid, and unsupported sources;
- HTTP, private-network, unsupported-capability, and notice warnings; and
- the action to be taken for an existing catalog.

Normal imports use an `Install` confirmation. A catalog exceeding either
1 MiB or 100 sources also displays a prominent large-catalog warning and
requires an additional explicit confirmation.

Catalog-level structural errors abort. Source-level errors exclude the
invalid sources and are shown in the preview. The user may install the valid
subset if at least one valid source remains.

### 10.3 Installed catalog management

`Installed source catalogs...` opens a minimal non-editor dialog listing:

- name, ID, revision, origin, installed time, and source count;
- whether a catalog is bundled or user-installed; and
- a Remove action for user-installed catalogs.

Editing, source creation, and automatic update controls are deferred.
Removal affects future selection only. Already generated map-side imagery XML
files continue to work.

### 10.4 Selecting and editing a source

Selecting a catalog source fills the name and URL presentation but retains the
resolved source object, including its custom grid and request behavior.

If the user edits the URL field, the selection detaches from the catalog and
returns to manual URL classification. Catalog selections are not inserted into
the five-entry manual recent-source list.

## 11. Validation and security

Catalogs are untrusted data. They are never executable configuration.

### 11.1 Transport limits

- Maximum downloaded/local JSON size: 10 MiB.
- Large-catalog warning threshold: 1 MiB.
- Maximum sources: 1,000.
- Large-catalog source warning threshold: 100.
- Maximum redirects: 5.
- Redirects may not switch to `file:` or another non-HTTP scheme.
- Bounded connection and total request timeouts.
- Content-Length is checked when present, and streaming is stopped at the
  hard byte limit when absent or inaccurate.

### 11.2 Parser and semantic limits

- Bounded nesting depth and string/URL lengths.
- Duplicate catalog or source IDs are invalid.
- Nonfinite numeric values are invalid.
- URL templates must contain exactly the placeholders required by their
  declared source type.
- Tile sizes, matrix sizes, levels, and limits must be internally consistent.
- CRS definitions must parse through Mapper's PROJ integration.
- Translation and affine operations must be reversible.
- Resource references must be declared and checksummed.
- Unsupported required semantics are never ignored.

### 11.3 Network and privacy behavior

The preview lists all source hostnames. HTTP, loopback, link-local, and private
network destinations are allowed only with a warning; they are legitimate for
club-hosted services and therefore cannot be rejected categorically.

Import does not contact source hosts. Selecting and adding a source causes the
same tile traffic that a manually entered source causes today.

Catalog strings are displayed as plain text. URLs shown as links are validated
and opened only after the normal explicit user action.

## 12. Identity, duplicates, and updates

Catalog and source identity must not depend on display names.

### 12.1 Catalog identity

The tuple `(catalog id, revision)` identifies a published snapshot.

- Same ID, revision, and SHA-256: exact reimport; no changes.
- Same ID and higher revision: preview added, changed, removed, invalid, and
  unsupported sources; offer atomic replacement.
- Same ID and lower revision: explicit downgrade warning.
- Same ID and revision but different hash: suspicious republish; require an
  explicit replacement confirmation and record the previous hash.

### 12.2 Source identity

Within a catalog, source ID is stable across revisions. Across catalogs,
Mapper computes:

1. a full canonical fingerprint including all defined source content; and
2. an operational fingerprint containing URL templates, scheme, tile matrix
   set, limits, request behavior, and registration.

An exact full fingerprint is skipped as a duplicate. An operational match
with different descriptive/notices metadata is reported as a potential
duplicate and kept distinct unless the user chooses otherwise in a later
management UI.

The same URL with a different grid, request behavior, or registration is not
a duplicate.

Canonical comparison normalizes recognized placeholder spelling, scheme and
host case, and default ports. It does not reorder query parameters or rewrite
the URL stored by the catalog.

The generated imagery XML filename currently hashes the URL. It must instead
hash the operational fingerprint so two corrected uses of the same endpoint
cannot collide.

## 13. Persistence

Bundled catalogs are Qt resources and are read-only.

User-installed catalog snapshots live below
`QStandardPaths::AppDataLocation`, for example:

```text
imagery-catalogs/
  <safe catalog key>/
    catalog.json
    state.json
```

The safe directory key is derived from the catalog ID rather than using the
untrusted ID directly as a path.

`catalog.json` is the exact installed snapshot. `state.json` records origin,
install/fetch time, SHA-256, ETag, Last-Modified, and prior snapshot metadata
needed for update reporting. Writes use `QSaveFile` and atomic replacement.
`QSettings` stores only lightweight UI ordering and enablement preferences.

URL catalogs are snapshots. Version 1 does not refresh them automatically.
Persisting ETag and Last-Modified makes a later explicit Reload action
straightforward without changing storage format.

Generated GDAL XML remains map-side and self-contained, as it is today.
Catalog updates and removal do not retroactively change existing maps.

## 14. Rendering integration

### 14.1 Runtime model

Introduce a catalog-level definition model and keep it separate from the
resolved runtime source:

- `ImageryCatalog`;
- `ImagerySourceDefinition`;
- `TileMatrixSetDefinition`;
- `ImageryRegistration`; and
- `OnlineImagerySource`, expanded as the resolved source used by the builder.

The catalog reader never emits GDAL XML directly. A resolver validates a
definition against application capabilities and constructs a runtime source.

### 14.2 Generic grid generation

Replace the Web-Mercator-specific builder path with:

1. convert the selected map extent to geographic coordinates using the map's
   current georeferencing;
2. project the extent into the source tile matrix CRS using `ProjTransform`;
3. apply the inverse registration when determining which nominal source tiles
   are required;
4. select the highest permitted tile matrix for the initial template;
5. snap the extent to that matrix's origin, cell size, tile dimensions, and
   matrix limits; and
6. emit the GDAL WMS data window, projection, request values, and cache entry.

Extent conversion samples more than two corners when the source projection is
nonlinear over the selected area. At minimum, corners and edge midpoints are
used, with failure if projection produces no finite bounding box.

The existing global Web Mercator math remains covered by regression tests but
becomes a standard matrix-set instance rather than a hardcoded special case.

### 14.3 Registration rendering

For `translation2d`, shift the georeferenced output data window while retaining
the nominal tile indexes used for requests.

Affine correction requires a GDAL VRT wrapper with a six-coefficient
geotransform. Grid correction requires a warped VRT or equivalent GDAL/PROJ
operation and stable access to the checked resource. Until those paths exist,
the resolver reports the required capability as unsupported and the chooser
disables the source.

## 15. Proposed source layout

Exact file names may be adjusted during implementation, but responsibilities
should remain separated:

```text
src/gdal/
  imagery_catalog.h/.cpp
  imagery_catalog_reader.h/.cpp
  imagery_catalog_store.h/.cpp
  imagery_tile_matrix_set.h/.cpp
  online_imagery_source.h
  online_imagery_template_builder.h/.cpp

src/gui/widgets/
  imagery_catalog_import_dialog.h/.cpp
  imagery_catalog_manager_dialog.h/.cpp
  online_template_dialog.h/.cpp

test/data/imagery-catalogs/
  puget-sound-example.oom-imagery.json
  invalid/

doc/
  imagery-source-catalog.schema.json
  imagery-source-catalog.md
```

No new third-party parser or networking dependency is required. Qt Core JSON,
Qt Network, `QStandardPaths`, and `QSaveFile` cover the implementation and are
already available in supported Mapper builds.

## 16. Example and test catalog

The first fixture is derived from sources already used by local Mapper
installations:

- King County Aerial 2023;
- King County Aerial 2025; and
- Pierce County imagery through its cached ArcGIS endpoint.

The fixture captures only source configuration and service metadata needed to
exercise the parser and builder. Automated tests never contact county servers.

The cases intentionally cover:

- standard WebMercatorQuad sources;
- service-advertised levels that may need a conservative usable maximum;
- a custom EPSG:2927 tile matrix set;
- non-global origin and extent;
- a referer;
- multiple empty-tile status codes; and
- two different source definitions sharing broadly similar ArcGIS URL
  structure.

The test fixture is not automatically a production bundled-source decision.
Bundled-source inclusion receives a separate review of reliability,
attribution, terms, and product policy.

## 17. Test plan

### 17.1 Parser and schema tests

- minimal valid catalog;
- complete catalog round trip without rewriting the installed snapshot;
- wrong format and unsupported document version;
- duplicate IDs and invalid revisions;
- invalid URL templates and forbidden URL/header forms;
- size, count, nesting, and string limits;
- unknown catalog- and source-level capabilities;
- strict unknown-field and namespaced-extension behavior; and
- invalid, singular, or nonfinite registration parameters.

### 17.2 Tile matrix and builder tests

- current Web Mercator coordinate and snapping regression cases;
- King County WebMercatorQuad XML generation;
- Pierce County EPSG:2927 origin, level, tile index, projection, referer, and
  empty-code generation;
- map extents crossing tile and matrix boundaries;
- CRS conversion failures and partially nonfinite projected extents;
- translation correction request indexes versus corrected output coordinates;
  and
- output filename differences when grid or registration differs for the same
  URL.

### 17.3 Store and identity tests

- exact reimport no-op;
- higher-revision update and removed-source reporting;
- lower-revision warning;
- same revision/different hash conflict;
- full and operational fingerprint duplicates;
- atomic installation and rollback on write failure;
- removal without touching generated templates; and
- safe paths for malicious or unusual catalog IDs.

### 17.4 Network import tests

Use a local deterministic HTTP server to cover:

- successful HTTPS-equivalent response handling;
- redirects and redirect limits;
- Content-Length and streaming hard limits;
- timeout and partial response;
- ETag and Last-Modified persistence;
- HTTP/private-host warnings; and
- rejection of redirects to non-HTTP schemes.

### 17.5 UI and release checks

- chooser grouping, disabled headings, and action rows;
- catalog selection does not enter manual recents;
- URL editing detaches a catalog selection;
- import summary counts and large-catalog confirmation;
- unsupported registered sources are disabled with an explanation;
- catalog persistence after restart; and
- Windows installed-build smoke: import local fixture, select King and Pierce,
  add templates, restart, remove catalog, and verify existing templates remain.

## 18. Implementation sequence

Implementation should be reviewable as focused commits:

1. **Format and model**: JSON Schema, catalog/source models, reader,
   validation, fingerprints, and fixtures.
2. **Generic tiling**: standard/inline matrix sets, source-CRS extent
   conversion, generic snapping, request options, and translation correction.
3. **Catalog store**: app-data snapshots, atomic update/removal, revision and
   duplicate analysis.
4. **Import and chooser UI**: file/URL load, preview, grouping, source handles,
   and minimal installed-catalog management.
5. **Bundled catalog and release QA**: move approved hardcoded presets into a
   read-only resource catalog, package the assets, and perform installer smoke
   testing.

The work should not alter unrelated build, packaging, template, or map-file
behavior. The manual URL path and existing generated XML behavior remain
available throughout the implementation.

## 19. Acceptance criteria for the first user build

The feature is ready for the first user when all of the following hold:

- a catalog imports from a local file and an HTTPS URL;
- the preview accurately reports sources, hostnames, duplicates, errors, and
  unsupported capabilities;
- the Puget Sound fixture installs with three selectable sources;
- King sources generate Web Mercator templates and Pierce generates its
  EPSG:2927 template with correct request behavior;
- a translation registration changes output georeferencing without changing
  nominal tile requests;
- restart preserves the installed catalog;
- exact reimport creates no entries;
- a new revision gives an explicit atomic update preview;
- removal deletes chooser entries but leaves generated templates intact;
- malformed, conflicting, or oversized imports make no partial storage
  changes;
- affine/grid registered sources can be installed but cannot be selected by a
  build that cannot apply them; and
- the Windows installed-build smoke test passes without requiring development
  files or network access to import the local fixture.

## 20. Review decisions requested

Review should explicitly confirm or change:

1. the custom JSON wrapper plus embedded OGC tile matrix set approach;
2. `.oom-imagery.json` and the proposed media type;
3. strict fields plus namespaced extensions rather than permissive unknown
   keys;
4. the distinction between a tile matrix set and a surveyed registration;
5. translation support in the first implementation with affine/grid
   capability-gated;
6. snapshot installation with no automatic refresh;
7. the 1 MiB/100-source warning and 10 MiB/1,000-source hard limits; and
8. keeping the Puget Sound fixture separate from the product's approved
   bundled catalog.
