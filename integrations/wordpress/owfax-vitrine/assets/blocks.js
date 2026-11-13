// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Blocks of the editor, equivalent to the shortcodes (WPP-020): the server renders
// them (ServerSideRender). No build step: plain wp.element.createElement.

'use strict';

(function (wp) {
  const el = wp.element.createElement;
  const __ = wp.i18n.__;
  const Inspector = wp.blockEditor.InspectorControls;
  const C = wp.components;
  const SSR = wp.serverSideRender;

  function block(name, title, description, controls) {
    wp.blocks.registerBlockType(name, {
      apiVersion: 2,
      title: title,
      description: description,
      category: 'media',
      icon: 'format-image',
      edit: function (props) {
        const blockProps = wp.blockEditor.useBlockProps();
        return el('div', blockProps,
          el(Inspector, null, el(C.PanelBody, { title: __('Settings', 'owfax-vitrine') }, controls(props))),
          el(SSR, { block: name, attributes: props.attributes }));
      },
      save: function () { return null; },
    });
  }

  function num(props, key, label, min, max) {
    return el(C.RangeControl, {
      label: label, min: min, max: max, value: props.attributes[key],
      onChange: function (v) { const a = {}; a[key] = v; props.setAttributes(a); },
    });
  }

  function text(props, key, label, help) {
    return el(C.TextControl, {
      label: label, help: help, value: props.attributes[key],
      onChange: function (v) { const a = {}; a[key] = v; props.setAttributes(a); },
    });
  }

  block('owfax/player', __('Weather fax replay', 'owfax-vitrine'),
    __('The latest chart replayed with its sound, or the latest ones in a loop.', 'owfax-vitrine'),
    function (p) {
      return [
        num(p, 'playlist', __('Charts in the loop', 'owfax-vitrine'), 1, 30),
        el(C.SelectControl, {
          key: 'rate', label: __('Speed', 'owfax-vitrine'), value: String(p.attributes.rate),
          options: [{ label: '×1', value: '1' }, { label: '×4', value: '4' }, { label: '×10', value: '10' }],
          onChange: function (v) { p.setAttributes({ rate: Number(v) }); },
        }),
        text(p, 'station', __('Station', 'owfax-vitrine'), __('Catalogue id, for example dwd-pinneberg; empty: all.', 'owfax-vitrine')),
      ];
    });

  block('owfax/gallery', __('Weather fax gallery', 'owfax-vitrine'),
    __('Thumbnails of the latest charts, with distance, date and quality.', 'owfax-vitrine'),
    function (p) {
      return [
        num(p, 'limit', __('Charts', 'owfax-vitrine'), 1, 100),
        text(p, 'station', __('Station', 'owfax-vitrine'), __('Catalogue id; empty: all, with a filter.', 'owfax-vitrine')),
      ];
    });

  block('owfax/chart', __('Weather fax chart', 'owfax-vitrine'),
    __('One chart, by its id.', 'owfax-vitrine'),
    function (p) {
      return [text(p, 'id', __('Chart id', 'owfax-vitrine'), '20261110T1007Z_DDK6_13882.5kHz')];
    });
})(window.wp);
