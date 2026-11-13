<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Small JSON Schema validator, for the keywords the package schemas use
// (docs/schemas/owfax-vitrine-0.1.schema.json): type, required, properties,
// additionalProperties, const, enum, items, minimum, maximum, exclusiveMinimum,
// minLength, pattern, oneOf and local $ref. No WordPress dependency (tests run it
// with plain PHP). Documents are decoded as objects (json_decode without assoc), so
// that {} and [] stay apart.

defined( 'OWFAX_VITRINE_STANDALONE' ) || defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Schema {
	/** @var object */
	private $root;

	/** @param object $schema Decoded schema. */
	public function __construct( $schema ) {
		$this->root = $schema;
	}

	public static function from_file( string $path ): self {
		$schema = json_decode( (string) file_get_contents( $path ) );
		if ( ! is_object( $schema ) ) {
			throw new RuntimeException( 'unreadable schema: ' . $path );
		}
		return new self( $schema );
	}

	/**
	 * Validates a decoded document.
	 *
	 * @param mixed $doc Document, decoded as objects.
	 * @return string[] Errors ("/path: message"), empty if valid.
	 */
	public function validate( $doc ): array {
		$errors = array();
		$this->check( $doc, $this->root, '', $errors );
		return $errors;
	}

	private function resolve( $schema ) {
		if ( is_object( $schema ) && isset( $schema->{'$ref'} ) ) {
			$ref = $schema->{'$ref'};
			if ( 0 !== strpos( $ref, '#/' ) ) {
				throw new RuntimeException( 'only local references: ' . $ref );
			}
			$node = $this->root;
			foreach ( explode( '/', substr( $ref, 2 ) ) as $part ) {
				$node = $node->{$part};
			}
			return $this->resolve( $node );
		}
		return $schema;
	}

	private static function type_of( $v ): string {
		if ( null === $v ) {
			return 'null';
		}
		if ( is_bool( $v ) ) {
			return 'boolean';
		}
		if ( is_int( $v ) ) {
			return 'integer';
		}
		if ( is_float( $v ) ) {
			return 'number';
		}
		if ( is_string( $v ) ) {
			return 'string';
		}
		if ( is_array( $v ) ) {
			return 'array';
		}
		return 'object';
	}

	private static function is_type( $v, string $type ): bool {
		$t = self::type_of( $v );
		if ( 'number' === $type ) {
			return 'number' === $t || 'integer' === $t;
		}
		if ( 'integer' === $type && 'number' === $t ) {
			return floor( $v ) === $v;  // 3.0 is an integer in JSON Schema
		}
		return $t === $type;
	}

	private function check( $v, $schema, string $path, array &$errors ): void {
		$schema = $this->resolve( $schema );
		if ( true === $schema ) {
			return;
		}
		if ( false === $schema ) {
			$errors[] = ( '' === $path ? '/' : $path ) . ': not allowed';
			return;
		}
		$at = '' === $path ? '/' : $path;
		if ( isset( $schema->type ) ) {
			$types = is_array( $schema->type ) ? $schema->type : array( $schema->type );
			$ok    = false;
			foreach ( $types as $type ) {
				$ok = $ok || self::is_type( $v, $type );
			}
			if ( ! $ok ) {
				$errors[] = $at . ': expected ' . implode( ' or ', $types ) . ', got ' . self::type_of( $v );
				return;
			}
		}
		if ( property_exists( $schema, 'const' ) && $v !== $schema->const ) {
			$errors[] = $at . ': must be ' . json_encode( $schema->const );
		}
		if ( isset( $schema->enum ) && ! in_array( $v, $schema->enum, true ) ) {
			$errors[] = $at . ': not one of ' . json_encode( $schema->enum );
		}
		if ( isset( $schema->oneOf ) ) {
			$matches = 0;
			foreach ( $schema->oneOf as $option ) {
				$sub = array();
				$this->check( $v, $option, $path, $sub );
				if ( ! $sub ) {
					++$matches;
				}
			}
			if ( 1 !== $matches ) {
				$errors[] = $at . ': must match exactly one schema of oneOf (' . $matches . ' do)';
			}
		}
		if ( is_int( $v ) || is_float( $v ) ) {
			if ( isset( $schema->minimum ) && $v < $schema->minimum ) {
				$errors[] = $at . ': below ' . $schema->minimum;
			}
			if ( isset( $schema->maximum ) && $v > $schema->maximum ) {
				$errors[] = $at . ': above ' . $schema->maximum;
			}
			if ( isset( $schema->exclusiveMinimum ) && $v <= $schema->exclusiveMinimum ) {
				$errors[] = $at . ': must be above ' . $schema->exclusiveMinimum;
			}
		}
		if ( is_string( $v ) ) {
			if ( isset( $schema->minLength ) && mb_strlen( $v, 'UTF-8' ) < $schema->minLength ) {
				$errors[] = $at . ': shorter than ' . $schema->minLength;
			}
			if ( isset( $schema->pattern ) &&
				1 !== preg_match( '/' . str_replace( '/', '\/', $schema->pattern ) . '/u', $v ) ) {
				$errors[] = $at . ': does not match ' . $schema->pattern;
			}
		}
		if ( is_array( $v ) && isset( $schema->items ) ) {
			foreach ( $v as $i => $item ) {
				$this->check( $item, $schema->items, $path . '/' . $i, $errors );
			}
		}
		if ( is_object( $v ) ) {
			foreach ( isset( $schema->required ) ? $schema->required : array() as $name ) {
				if ( ! property_exists( $v, $name ) ) {
					$errors[] = $at . ': missing ' . $name;
				}
			}
			$props = isset( $schema->properties ) ? $schema->properties : new stdClass();
			foreach ( get_object_vars( $v ) as $name => $value ) {
				if ( property_exists( $props, $name ) ) {
					$this->check( $value, $props->{$name}, $path . '/' . $name, $errors );
				} elseif ( property_exists( $schema, 'additionalProperties' ) ) {
					$this->check( $value, $schema->additionalProperties, $path . '/' . $name, $errors );
				}
			}
		}
	}
}
