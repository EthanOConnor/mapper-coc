# Imagery source catalogs: design and implementation specification

Status: draft for review

This document specifies a portable catalog of online imagery sources for
OpenOrienteering Mapper. It covers the interchange format, import and
installation behavior, validation, source identity, custom tile grids, and
future surveyed registration corrections.

The first implementation is intended for **Mapper (COC Base)**, the
conservative preview line in contrast to `full-speed-ahead`. The short
user-facing name reads as an edition of Mapper rather than a separate COC-owned
product. Formal attribution, licensing, and upstream references continue to
use **OpenOrienteering Mapper**. The format and implementation are deliberately
OpenOrienteering-neutral. Nothing in the core format is tied to COC, a
particular imagery provider, or a particular jurisdiction.

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

- a versioned JSON catalog format and an informative JSON Schema for authoring
  tools;
- strict parsing and semantic validation;
- standard and inline OGC tile matrix sets that satisfy the dyadic pyramid
  profile the GDAL WMS/TMS renderer can represent;
- local-file and HTTP(S) catalog import;
- an import preview, duplicate/conflict reporting, and large-catalog warning;
- per-user persistent catalog snapshots and catalog removal;
- catalog entries in the existing online imagery source chooser;
- generic source-CRS tile-grid cropping, including a synthetic EPSG:2927 grid
  derived from the Pierce County service used by the test fixture;
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
- checksum-based detection of placeholder/empty tile images, which the first
  GDAL WMS/TMS rendering path cannot express;
- general WMS, WMTS, OGC API Tiles, or STAC service discovery; and
- execution of affine or grid-shift registrations until the required
  GDAL VRT/warp path is implemented and tested.

The schema defines affine and grid-shift operations now so that catalog
authors do not need a later incompatible format. A source requiring an
operation unsupported by the running Mapper build is retained but disabled.
It must never be rendered using its uncorrected nominal georeferencing.

### 2.3 Threat model and priorities

Catalogs are untrusted data, but version 1 does not attempt to solve every
possible trust or privacy concern around remote imagery.

The first implementation must prevent consequences outside the intended
imagery workflow: code execution, arbitrary local file access, credential
disclosure, XML or HTTP-header injection, unexpected requests to local or
link-local services during catalog fetch, persistent state corruption,
unbounded resource consumption, and silently incorrect georeferencing.

The first implementation does not try to hide requested map extents from an
imagery provider, authenticate the pixels returned by that provider, detect a
provider serving unexpected imagery, establish publisher reputation, or give
repeated warnings about hosts the user intentionally installed. Catalog
signatures and stronger content provenance remain possible future work, but
they are not release gates for this feature.

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

The OGC object is the interchange model, not a promise that the first renderer
can draw every valid OGC pyramid. GDAL's WMS/TMS mini-driver exposes one tile
size and a sequence of overview levels, each exactly half the preceding
resolution. Version 1 therefore defines a renderable dyadic profile in
section 7.3 and capability-gates valid OGC matrix sets outside that profile.
The GDAL WMTS driver is not a general escape hatch for plain XYZ endpoints: it
selects matrix sets advertised by a WMTS GetCapabilities document.

The filename extension is `.oic`. The proposed media type is
`application/vnd.openorienteering.imagery-catalog+json`.
That media type is unregistered and is only a useful content hint. Mapper
dispatches on the required `format` and `version` members, never on a filename
suffix or HTTP Content-Type alone.

The `org.openorienteering.imagery-catalog` format identifier and vendor media
type are aspirational until accepted by the upstream OpenOrienteering project;
experimental use by the COC fork does not claim upstream or IANA registration.
An upstream review may rename them before format version 1 is declared stable.

The term **package** is reserved for a future archive containing the same
catalog manifest and referenced binary resources such as displacement grids.
The first implementation imports the JSON catalog only.

References:

- <https://github.com/osmlab/editor-layer-index>
- <https://github.com/mapbox/tilejson-spec/tree/master/3.0.0>
- <https://www.ogc.org/standards/tms/>
- <https://schemas.opengis.net/tms/2.0/json/>
- <https://github.com/radiantearth/stac-spec>
- <https://gdal.org/en/stable/drivers/raster/wms.html>
- <https://gdal.org/en/stable/drivers/raster/wmts.html>

## 6. Catalog document

Catalog files are UTF-8 JSON. The hand-written C++ reader and semantic
validator are normative for Mapper behavior. The published JSON Schema is
informative: it supplies editor completion, catches structural authoring
errors, and is tested against the same fixtures, but it is not loaded by the
application and cannot express all cross-field, CRS, or rendering checks.

The schema uses `additionalProperties: false`, and the C++ validator rejects
the same unknown members. Future experimental data belongs under a namespaced
`extensions` object rather than as arbitrary top-level members. CI runs valid
and invalid fixtures through the C++ validator and a pinned JSON Schema
validator to detect drift; if they disagree, the C++ validator determines
application behavior and the schema is fixed.

An illustrative catalog is:

```json
{
  "format": "org.openorienteering.imagery-catalog",
  "version": 1,
  "id": "org.example.mapping.imagery-demo",
  "revision": 1,
  "name": "Example imagery catalog",
  "description": "Synthetic definitions for catalog and grid testing.",
  "publisher": {
    "name": "Example Mapping Club",
    "url": "https://www.example.org/"
  },
  "updated": "2026-07-09",
  "sources": [
    {
      "id": "aerial-2025",
      "name": "Example Aerial 2025",
      "type": "raster-tiles",
      "tiles": [
        "https://tiles.example.test/aerial-2025/{z}/{y}/{x}"
      ],
      "scheme": "xyz",
      "tileMatrixSetURI": "http://www.opengis.net/def/tilematrixset/OGC/1.0/WebMercatorQuad",
      "minTileMatrix": "0",
      "maxTileMatrix": "20",
      "request": {
        "emptyHttpStatusCodes": [404]
      },
      "notices": {
        "sourceUrl": "https://www.example.org/imagery/aerial-2025"
      }
    }
  ]
}
```

The normative example uses reserved example domains. Real-source examples are
kept separate from the format definition and do not imply that imagery is
approved for every use or should be bundled in the product.

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
: Positive integer that increases across publications carrying the same
  catalog `id`. Changing catalog content should increment it.

`name`
: Plain-text user-facing catalog name.

`sources`
: Nonempty array of source definitions. Source IDs must be unique within a
  catalog.

### 6.2 Optional catalog members

- `$schema`: informational URI for authoring tools; Mapper never dereferences
  it and no canonical OpenOrienteering URL is assumed until the upstream
  project chooses and publishes one;
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

`elevation` includes raster elevation products and derivatives commonly used
for orienteering, including DEMs, hillshade, slope imagery, and rasterized
LiDAR products. Version 1 does not attempt to describe point-cloud access.

All user-facing strings are plain text. HTML supplied by a catalog is never
rendered.

### 7.2 Tile access

`tiles` is a nonempty array of absolute HTTP or HTTPS URL templates.
Equivalent endpoints may be listed for load distribution, but each must
return the same content for the same logical tile. Other URL schemes are
invalid in version 1.

Version 1 recognizes `{z}`, `{x}`, and `{y}`. In the renderable dyadic profile,
`{z}` is a decimal zoom number rather than an arbitrary OGC matrix identifier.
The parser also accepts Mapper's current
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

- `referer`: an absolute HTTP(S) URL containing no control characters,
  including carriage return or line feed;
- `emptyHttpStatusCodes`: a unique array of integer HTTP status codes.

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
inline definitions that satisfy the `tile-matrix-set.dyadic.v1` profile.
Referencing an unknown URI is an unsupported capability, not permission to
assume Web Mercator.

Inline validation checks:

- a version 1 CRS reference, limited to an `EPSG:<integer>` code or the
  equivalent OGC CRS URI;
- unique matrix identifiers;
- finite positive cell sizes;
- finite origins;
- positive tile and matrix dimensions;
- consistent axis and origin semantics;
- limits within the corresponding matrix dimensions; and
- an unambiguous ordering for `minTileMatrix` and `maxTileMatrix`.

The dyadic rendering profile additionally requires:

- decimal integer matrix identifiers that start at zero, form a contiguous
  sequence, and are the values substituted for `{z}`;
- constant tile width and height across the usable sequence;
- one common origin, origin corner, axis order, and row orientation;
- square pixels;
- each successively finer matrix to have half the preceding cell size, within
  a documented floating-point tolerance;
- each successively finer full matrix width and height to be twice the
  preceding values; and
- no variable-width tile matrices.

`tileMatrixLimits` may restrict the available portion of each full matrix and
need not itself double exactly. Base matrix dimensions may be greater than one,
which permits regional ArcGIS caches such as Pierce County.

An inline object can be valid OGC JSON while failing this renderer profile.
Such a source is retained but disabled as requiring a future
`tile-matrix-set.nondyadic.v1` capability. It does not proceed to GDAL and does
not fail later while a user is adding a template. The unsupported fixture set
contains a syntactically valid non-dyadic pyramid to pin this boundary.

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
Direction is never implicit. Version 1 permits exactly
`source-to-corrected`; any other value is invalid rather than being guessed or
automatically inverted.

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
Version 1 permits exactly `unit: crs`, meaning the shared source/target CRS
linear unit. Thus an EPSG:2927 translation is expressed in that CRS's US survey
feet, not metres. Explicit unit conversion requires a later capability.
Translation is reversible and can be implemented in the first release by
shifting the generated GDAL data window.

### 8.2 Affine correction

`affine2d` uses named parameters matching the common affine equation:

```text
x' = xoff + s11*x + s12*y
y' = yoff + s21*x + s22*y
```

The determinant must be finite and nonzero. This aligns with the parameters
used by PROJ's affine operation and EPSG affine/similarity methods while
remaining readable.

Affine operations likewise use exactly `unit: crs` in version 1. `xoff` and
`yoff` are in CRS units; `s11`, `s12`, `s21`, and `s22` are dimensionless.

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
- `tile-matrix-set.dyadic.v1`;
- `tile-matrix-set.nondyadic.v1` (reserved; unsupported by the first
  renderer);
- `registration.translation2d.v1`;
- `registration.affine2d.v1`; and
- `registration.grid-shift.v1`.

Unknown catalog-level required capabilities abort import. Unknown or
unsupported source-level required capabilities retain the catalog but disable
that source with a clear explanation.

The reader derives required rendering capabilities from source content as
well as checking an explicit `requires` array. An author cannot omit
`tile-matrix-set.nondyadic.v1` and thereby make a non-dyadic matrix set appear
renderable.

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
- A fetch that begins with HTTPS may not redirect to HTTP. The downgrade is
  rejected and reported; a user who intends to load the HTTP destination can
  enter it explicitly and accept the normal direct-HTTP warning.
- Scheme, user information, resolved destination class, and byte limits are
  revalidated before every redirect hop. A public catalog URL may not silently
  redirect to a loopback, link-local, or private address; the fetch stops and
  reports the destination. A user who intends to load that private catalog can
  enter its final URL explicitly and accept the normal private-host warning.
- Bounded connection and total request timeouts.
- Content-Length is checked when present, and streaming is stopped at the
  hard byte limit when absent or inaccurate.

### 11.2 Parser and semantic limits

- Maximum nesting depth: 64.
- Bounded string and URL lengths.
- Maximum tile URL templates per source: 8.
- Maximum tile matrices per matrix set: 64.
- Maximum vertices across one coverage geometry: 10,000.
- Maximum empty-tile HTTP codes per source: 32.
- Duplicate catalog or source IDs are invalid.
- Nonfinite numeric values are invalid.
- URL templates must contain exactly the placeholders required by their
  declared source type.
- Tile sizes, matrix sizes, levels, and limits must be internally consistent.
- CRS references are restricted to EPSG codes and equivalent OGC CRS URIs and
  must parse through Mapper's PROJ integration. Version 1 does not accept
  catalog-supplied PROJ strings, pipelines, WKT, or PROJJSON.
- Translation and affine operations must be reversible.
- Resource references must be declared and checksummed.
- Unsupported required semantics are never ignored.

Catalog validation does not alter Mapper's process-wide PROJ network policy.
If a user or packager has enabled PROJ network access, ordinary EPSG
transformations may follow that existing policy; the catalog format itself
cannot name a grid URL or introduce an arbitrary PROJ pipeline.

Every catalog-derived value written into GDAL XML is XML-escaped, including
server URLs, projection identifiers, referer values, formats, and future textual
fields. The builder follows its existing `toHtmlEscaped()` practice rather
than interpolating catalog strings directly.

### 11.3 Network behavior and privacy boundary

The preview lists all source hostnames. HTTP, loopback, link-local, and private
network destinations are allowed only with a warning; they are legitimate for
club-hosted services and therefore cannot be rejected categorically.

Import does not contact source hosts. Selecting and adding a source causes the
same tile traffic that a manually entered source causes today.

The import preview is the one host disclosure and confirmation point. Mapper
does not warn again every time an intentionally installed source is selected.
The host list is informational, not a phishing or publisher-authentication
mechanism.

Once used, a source operator can observe tile requests and infer the requested
area, and can return imagery different from what the catalog author expected.
Preventing those behaviors is outside the version 1 threat model. HTTPS still
provides ordinary transport integrity and is tested as a functional release
requirement.

Catalog strings are displayed as plain text. URLs shown as links are validated
and opened only after the normal explicit user action.

## 12. Identity, duplicates, and updates

Catalog and source identity must not depend on display names.

### 12.1 Catalog identity

The tuple `(catalog id, revision)` identifies a published snapshot.
The catalog SHA-256 is computed over the exact downloaded/local UTF-8 bytes,
before parsing or reformatting.

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
host case, the host's ASCII/ACE representation, and default ports. It does not
normalize path/query percent encodings, reorder query parameters, or rewrite
the URL stored by the catalog. Duplicate detection is a convenience and update
aid, not a trust or security boundary; conservative normalization may report
two equivalent spellings separately rather than risk changing request
semantics.

### 12.3 Fingerprint encoding

Source fingerprints are lowercase SHA-256 digests of UTF-8 JSON canonicalized
with the JSON Canonicalization Scheme (JCS), RFC 8785. JCS fixes object-key
ordering, string escaping, whitespace, literals, and IEEE 754 number
serialization, so equivalent spellings such as `-0.42` and `-4.2e-1` produce
the same bytes after parsing. Catalog JSON must satisfy the I-JSON constraints
required by JCS; duplicate object member names, invalid Unicode, and nonfinite
or non-IEEE-754-representable numbers are invalid.
Duplicate member detection happens before construction of any JSON DOM that
might discard all but one value for a repeated key.

Before JCS serialization, the validator builds a normalized semantic source
object:

- recognized URL placeholder aliases use `{z}`, `{x}`, and `{y}`;
- URLs use the conservative normalization in section 12.2;
- format defaults are materialized;
- set-like arrays such as `requires` and `emptyHttpStatusCodes` are sorted and
  deduplicated, while order-significant arrays such as `tiles` and geometry
  coordinates retain document order; and
- a known `tileMatrixSetURI` is replaced by the exact bundled matrix-set
  definition used by the renderer, so URI and equivalent inline forms have the
  same operational identity.

The full fingerprint hashes a wrapper containing `fingerprintVersion: 1` and
this complete normalized source object, including its source ID and descriptive
metadata. The operational fingerprint hashes a synthetic object containing
exactly:

- `fingerprintVersion`, initially integer `1`;
- source `type`;
- normalized `tiles`, `scheme`, and `format`;
- the resolved tile matrix set, `minTileMatrix`, `maxTileMatrix`, and
  `tileMatrixLimits`;
- supported `request` behavior;
- `registration`; and
- required operational capabilities.

Name, description, category, dates, coverage, notices, publisher/catalog
identity, and non-operational extensions are excluded from the operational
fingerprint. A future change to normalization or field membership increments
`fingerprintVersion`; it does not silently reinterpret stored version 1
digests.

Reference: <https://www.rfc-editor.org/rfc/rfc8785.html>

The generated imagery XML filename currently hashes the URL. It must instead
hash the operational fingerprint so two corrected uses of the same endpoint
cannot collide. State/index data stores the complete 64-character digest. A
filename initially uses the first 12 hexadecimal characters; if an existing
candidate has a different full digest, the prefix is extended in four-character
increments until unique, up to the complete digest. An unrelated file is never
overwritten merely because a shortened prefix collides.

This filename change is forward-only. Existing URL-hash XML sidecars are not
renamed or deleted automatically, because they may still be referenced by map
files. Re-adding the same source after upgrade can therefore create one new
fingerprint-named sidecar alongside a legacy sidecar. Cleanup or equivalence-
based migration is separate future work.

Generated-file identity and tile-cache identity are intentionally different.
Registration belongs in the generated filename because it changes
georeferencing. It does not change the bytes returned for a nominal tile
request. The builder continues to emit GDAL's default `<Cache />` without a
registration-specific path; two registrations of the same endpoint may share
the normal request cache rather than downloading the same tiles twice.

## 13. Persistence

Bundled catalogs are Qt resources and are read-only.

User-installed catalog snapshots live below
`QStandardPaths::AppDataLocation`, for example:

```text
imagery-catalogs/
  <safe catalog key>/
    catalog.oic
    state.json
```

The safe directory key is derived from the catalog ID rather than using the
untrusted ID directly as a path.

`catalog.oic` is the exact installed snapshot. `state.json` records origin,
install/fetch time, SHA-256, ETag, Last-Modified, and prior snapshot metadata
needed for update reporting, together with fingerprint version and complete
source digests used by the installed index. Writes use `QSaveFile` and atomic
replacement. `QSettings` stores only lightweight UI ordering and enablement
preferences.

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

### 14.2 Generic dyadic grid generation

Replace the Web-Mercator-specific builder path with:

1. convert the selected map extent to geographic coordinates using the map's
   current georeferencing;
2. select the highest permitted matrix in the validated dyadic sequence;
3. project the extent into the source/target frame using `ProjTransform`;
4. apply the inverse registration when determining which nominal source tiles
   are required;
5. snap the extent to that matrix's origin, cell size, tile dimensions, and
   matrix limits; and
6. emit an XML-escaped GDAL WMS/TMS data window, projection, request values,
   explicit `TileLevel`/`OverviewCount` corresponding to the source's maximum
   and minimum usable zooms, and the normal default cache element.

For the dyadic TMS mini-driver, `TileLevel` is the maximum usable decimal zoom
and `OverviewCount` is `maxTileMatrix - minTileMatrix`. This prevents GDAL from
inventing requests below the source's declared minimum while keeping `{z}`
aligned with the catalog's matrix identifiers.

Extent conversion adaptively subdivides each boundary edge when the source
projection is nonlinear. Subdivision continues until the projected midpoint
deviates from its projected chord by no more than one quarter of a source
pixel at the selected matrix, subject to a bounded recursion/sample limit.
Conversion fails rather than underestimating coverage if no finite bounding
box meeting the tolerance can be produced.

The existing global Web Mercator math remains covered by regression tests but
becomes a standard matrix-set instance rather than a hardcoded special case.
Sources requiring a non-dyadic capability do not reach this builder path.

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
  imagery_json_canonicalizer.h/.cpp
  imagery_tile_matrix_set.h/.cpp
  online_imagery_source.h
  online_imagery_template_builder.h/.cpp

src/gui/widgets/
  imagery_catalog_import_dialog.h/.cpp
  imagery_catalog_manager_dialog.h/.cpp
  online_template_dialog.h/.cpp

test/data/imagery-catalogs/
  valid/
    custom-dyadic-epsg2927.oic
  invalid/
  unsupported/
    non-dyadic-matrix-set.oic

doc/
  imagery-source-catalog.schema.json
  imagery-source-catalog.md
```

No new third-party runtime parser or networking dependency is required. Qt
Core JSON, Qt Network, `QStandardPaths`, and `QSaveFile` cover the application
implementation and are already available in supported Mapper builds. A pinned
schema validator is a CI/development tool only; the C++ validator remains
normative.

The RFC 8785 encoder is isolated behind the canonicalizer component and tested
with the RFC's property-ordering and IEEE 754 vectors. Whether that small
component is implemented locally or imported under the project's normal
third-party policy is an upstream implementation decision; its output contract
is fixed by RFC 8785 rather than by Qt's non-normative JSON serialization.

## 16. Example and test catalog

The first source definitions are derived from sources already used by local
Mapper installations:

- King County Aerial 2023;
- King County Aerial 2025; and
- Pierce County imagery through its cached ArcGIS endpoint.

The upstream-candidate automated fixture copies the relevant numeric geometry
and request behavior but uses neutral names and `example.test` URLs. It
captures only the configuration needed to exercise the parser and builder and
never contacts county servers. This keeps provider policy and regional product
choices out of the upstream core tests.

A separate fork-level Puget Sound example catalog may contain the actual King
and Pierce definitions for manual/release testing after a source-policy review.
That is the compact real-world example requested for COC installations, but it
is not required for upstream acceptance and is not contacted by automated
tests.

The cases intentionally cover:

- standard WebMercatorQuad sources;
- service-advertised levels that may need a conservative usable maximum;
- a custom EPSG:2927 tile matrix set;
- non-global origin and extent;
- a referer;
- multiple empty-tile status codes; and
- two different source definitions sharing broadly similar ArcGIS URL
  structure.

An additional unsupported fixture is valid OGC JSON but deliberately
non-dyadic. It verifies that the catalog installs with that source disabled and
that no GDAL XML is generated for it.

The test fixture is not automatically a production bundled-source decision.
Bundled-source inclusion receives a separate review of reliability,
attribution, terms, and product policy.

## 17. Test plan

### 17.1 C++ parser and validator tests

- minimal valid catalog;
- complete catalog round trip without rewriting the installed snapshot;
- wrong format and unsupported document version;
- duplicate IDs and invalid revisions;
- invalid URL templates and forbidden URL/header forms;
- size, source count, nesting, string, URL mirror, matrix, resource, and
  coverage-vertex limits;
- unknown catalog- and source-level capabilities;
- strict unknown-field and namespaced-extension behavior;
- duplicate object member names, invalid Unicode, and invalid JCS numbers;
- rejection of the deferred `emptyTileChecksums` field in version 1; and
- invalid direction/unit values, singular affine transforms, and nonfinite
  registration parameters.

### 17.2 Informative schema tests

- every structurally valid fixture passes the pinned JSON Schema validator;
- every fixture invalid for a schema-expressible reason fails it;
- unsupported-but-well-formed fixtures pass structural schema validation; and
- C++ semantic tests separately cover CRS resolution, dyadic rendering,
  cross-field consistency, and numerical invertibility.

### 17.3 Tile matrix and builder tests

- current Web Mercator coordinate and snapping regression cases;
- a synthetic WebMercatorQuad XML generation case matching the King County
  geometry;
- a synthetic EPSG:2927 origin, level, tile index, projection, referer, and
  empty-code case matching the Pierce County geometry;
- a valid non-dyadic OGC matrix set rejected as unsupported by the renderer;
- dyadic tolerance, noncontiguous level, varying tile size, origin, and matrix
  growth failures;
- map extents crossing tile and matrix boundaries;
- CRS conversion failures and partially nonfinite projected extents;
- translation correction request indexes versus corrected output coordinates;
- XML escaping and referer control-character rejection;
- output filename differences when grid or registration differs for the same
  URL; and
- registration differences do not create a custom GDAL cache path.

### 17.4 Store and identity tests

- exact reimport no-op;
- higher-revision update and removed-source reporting;
- lower-revision warning;
- same revision/different hash conflict;
- RFC 8785 canonicalization vectors and stable SHA-256 output;
- equivalent key order, numeric notation, placeholder spelling, and known
  URI/inline matrix-set forms produce identical intended fingerprints;
- descriptive-only changes alter the full fingerprint but not the operational
  fingerprint;
- operational changes alter both expected identities;
- filename prefixes extend safely under a forced 12-character collision;
- a legacy URL-hash sidecar is preserved when a new fingerprint sidecar is
  created;
- atomic installation and rollback on write failure;
- removal without touching generated templates; and
- safe paths for malicious or unusual catalog IDs.

### 17.5 Network import tests

Use local deterministic HTTP and test-TLS servers to cover:

- successful HTTP and TLS response handling;
- redirects, redirect limits, and per-hop scheme/address revalidation;
- rejection of HTTPS-to-HTTP redirect downgrade;
- Content-Length and streaming hard limits;
- timeout and partial response;
- ETag and Last-Modified persistence;
- HTTP/private-host warnings; and
- rejection of redirects to non-HTTP schemes.

### 17.6 UI and release checks

- chooser grouping, disabled headings, and action rows;
- catalog selection does not enter manual recents;
- URL editing detaches a catalog selection;
- import summary counts and large-catalog confirmation;
- unsupported registered sources are disabled with an explanation;
- catalog persistence after restart; and
- Windows installed-build smoke: import a local fixture, import a small catalog
  from a real public HTTPS URL using `QNetworkAccessManager`, select the fork's
  King and Pierce examples, add templates, restart, remove the catalog, and
  verify existing templates remain. The existing GDAL tile TLS smoke remains
  separate because catalog import and imagery tiles use different network
  stacks.

## 18. Upstream and implementation sequence

### 18.1 Upstream boundary

The format, C++ reader/validator, synthetic fixtures, dyadic renderer, catalog
store, and generic dialogs are upstream-candidate work. They must remain free
of COC branding, provider allowlists, club-specific paths, and release-channel
assumptions. UI strings use Qt translation facilities, and the existing manual
URL workflow remains available.

Catalog/rendering code remains conditional on Mapper's existing GDAL feature
boundary. The implementation should not make GDAL or online access mandatory
for otherwise supported Mapper builds.

The real Puget Sound catalog, the choice of which sources COC ships, and COC
installer/release smoke automation are fork integration. Keep those in
separate commits from the upstream-candidate core so they can be changed or
dropped without rewriting the format implementation. A product source policy
is not encoded in the interchange schema.

### 18.2 Commit sequence

Implementation should be reviewable as focused commits:

1. **Format and model**: informative JSON Schema, catalog/source models,
   normative C++ validation, fingerprints, and synthetic valid/invalid/
   unsupported fixtures.
2. **Generic tiling**: standard/inline dyadic matrix sets, source-CRS extent
   conversion, generic snapping, request options, and translation correction.
3. **Catalog store**: app-data snapshots, atomic update/removal, revision and
   duplicate analysis.
4. **Import and chooser UI**: file/URL load, preview, grouping, source handles,
   and minimal installed-catalog management.
5. **Fork catalog and release QA**: build the separately reviewed COC catalog,
   package approved assets, and perform local plus real-HTTPS installer smoke
   testing.

The work should not alter unrelated build, packaging, template, or map-file
behavior. The manual URL path and existing generated XML behavior remain
available throughout the implementation.

## 19. Acceptance criteria for the first user build

The feature is ready for the first user when all of the following hold:

- a catalog imports from a local file and an HTTPS URL;
- the preview accurately reports sources, hostnames, duplicates, errors, and
  unsupported capabilities;
- the fork's Puget Sound example catalog installs with three selectable
  sources;
- King sources generate Web Mercator templates and Pierce generates its
  EPSG:2927 template with correct request behavior;
- a syntactically valid non-dyadic source is retained but disabled before it
  reaches GDAL;
- a translation registration changes output georeferencing without changing
  nominal tile requests;
- restart preserves the installed catalog;
- exact reimport creates no entries;
- semantically equivalent JSON key order and numeric spelling produce stable
  full and operational fingerprints;
- a new revision gives an explicit atomic update preview;
- removal deletes chooser entries but leaves generated templates intact;
- malformed, conflicting, or oversized imports make no partial storage
  changes;
- affine/grid registered sources can be installed but cannot be selected by a
  build that cannot apply them; and
- the Windows installed-build smoke test passes for both local import and one
  real public HTTPS catalog import, while automated source tests remain
  independent of county-server availability.

## 20. Review decisions requested

Review should explicitly confirm or change:

1. the custom JSON wrapper plus embedded OGC tile matrix set approach, with a
   dyadic first-renderer profile and capability-gated non-dyadic sets;
2. `.oic` and the unregistered, non-dispatching media type hint;
3. a normative C++ validator, informative CI-checked JSON Schema, strict
   fields, and namespaced extensions;
4. SHA-256 fingerprints over versioned RFC 8785 canonical normalized source
   objects;
5. the distinction between a tile matrix set and a surveyed registration;
6. translation support in the first implementation with affine/grid
   capability-gated;
7. snapshot installation with no automatic refresh;
8. the 1 MiB/100-source warning and 10 MiB/1,000-source hard limits;
9. keeping synthetic upstream fixtures, the real Puget Sound example, and the
   product's approved bundled catalog as separate concerns;
10. the high-consequence-only version 1 threat model; and
11. the split between upstream-candidate implementation commits and
    COC-specific catalog/release integration.
