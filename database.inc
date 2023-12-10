<?php

/**
 * @file
 * Database interface code for PostgreSQL database servers.
 */

/**
 * @addtogroup database
 * @{
 */

/**
 * The name by which to obtain a lock for retrive the next insert id.
 */
define('POSTGRESQL_NEXTID_LOCK', 1000);

class DatabaseConnection_pgsql extends DatabaseConnection {

  public function __construct(array $connection_options = array()) {
    // This driver defaults to transaction support, except if explicitly passed FALSE.
    $this->transactionSupport = !isset($connection_options['transactions']) || ($connection_options['transactions'] !== FALSE);

    // Transactional DDL is always available in PostgreSQL,
    // but we'll only enable it if standard transactions are.
    $this->transactionalDDLSupport = $this->transactionSupport;

    // Default to TCP connection on port 5432.
    if (empty($connection_options['port'])) {
      $connection_options['port'] = 5432;
    }

    // PostgreSQL in trust mode doesn't require a password to be supplied.
    if (empty($connection_options['password'])) {
      $connection_options['password'] = NULL;
    }
    // If the password contains a backslash it is treated as an escape character
    // http://bugs.php.net/bug.php?id=53217
    // so backslashes in the password need to be doubled up.
    // The bug was reported against pdo_pgsql 1.0.2, backslashes in passwords
    // will break on this doubling up when the bug is fixed, so check the version
    //elseif (phpversion('pdo_pgsql') < 'version_this_was_fixed_in') {
    else {
      $connection_options['password'] = str_replace('\\', '\\\\', $connection_options['password']);
    }

    $this->connectionOptions = $connection_options;

    $dsn = 'pgsql:host=' . $connection_options['host'] . ' dbname=' . $connection_options['database'] . ' port=' . $connection_options['port'];

    // Allow PDO options to be overridden.
    $connection_options += array(
      'pdo' => array(),
    );
    $connection_options['pdo'] += array(
      // Prepared statements are most effective for performance when queries
      // are recycled (used several times). However, if they are not re-used,
      // prepared statements become ineffecient. Since most of Drupal's
      // prepared queries are not re-used, it should be faster to emulate
      // the preparation than to actually ready statements for re-use. If in
      // doubt, reset to FALSE and measure performance.
      PDO::ATTR_EMULATE_PREPARES => TRUE,
      // Convert numeric values to strings when fetching.
      PDO::ATTR_STRINGIFY_FETCHES => TRUE,
    );
    parent::__construct($dsn, $connection_options['username'], $connection_options['password'], $connection_options['pdo']);

    // Fallback to pointing to ourselves if we're running in Backdrop < 1.19.0.
    if (empty($this->pdo)) {
      $this->pdo = $this;
    }

    // Force PostgreSQL to use the UTF-8 character set by default.
    $this->pdo->exec("SET NAMES 'UTF8'");

    // Execute PostgreSQL init_commands.
    if (isset($connection_options['init_commands'])) {
      $this->pdo->exec(implode('; ', $connection_options['init_commands']));
    }
  }

  public function prepareQuery($query) {
    // mapConditionOperator converts LIKE operations to ILIKE for consistency
    // with MySQL. However, Postgres does not support ILIKE on bytea (blobs)
    // fields.
    // To make the ILIKE operator work, we type-cast bytea fields into text.
    // Only do this for queries with a WHERE clause, which excludes CREATE TABLE
    // LIKE queries.
    // @todo This workaround only affects bytea fields, but the involved field
    //   types involved in the query are unknown, so there is no way to
    //   conditionally execute this for affected queries only.
    $query = stripos($query, 'where') ? preg_replace('/\b:?([^ ]+(\(.*\))?) +(I*LIKE|NOT +I*LIKE) /iU', '${1}::text ${3} ', $query) : $query;
    return parent::prepareQuery($query);
  }

  public function query($query, array $args = array(), $options = array()) {

    $options += $this->defaultOptions();

    // The PDO PostgreSQL driver has a bug which
    // doesn't type cast booleans correctly when
    // parameters are bound using associative
    // arrays.
    // See http://bugs.php.net/bug.php?id=48383
    foreach ($args as &$value) {
      if (is_bool($value)) {
        $value = (int) $value;
      }
    }

    try {
      if ($query instanceof DatabaseStatementInterface) {
        $stmt = $query;
        $stmt->execute(NULL, $options);
      }
      else {
        $this->expandArguments($query, $args);
        $stmt = $this->prepareQuery($query);
        $stmt->execute($args, $options);
      }

      switch ($options['return']) {
        case Database::RETURN_STATEMENT:
          return $stmt;
        case Database::RETURN_AFFECTED:
          return $stmt->rowCount();
        case Database::RETURN_INSERT_ID:
          return $this->pdo->lastInsertId($options['sequence_name']);
        case Database::RETURN_NULL:
          return;
        default:
          throw new PDOException('Invalid return directive: ' . $options['return']);
      }
    }
    catch (PDOException $e) {
      if ($options['throw_exception']) {
        // Add additional debug information.
        if ($query instanceof DatabaseStatementInterface) {
          $e->query_string = $stmt->getQueryString();
        }
        else {
          $e->query_string = $query;
        }
        $e->args = $args;
        throw $e;
      }
      return NULL;
    }
  }

  public function queryRange($query, $from, $count, array $args = array(), array $options = array()) {
    return $this->query($query . ' LIMIT ' . (int) $count . ' OFFSET ' . (int) $from, $args, $options);
  }

  public function queryTemporary($query, array $args = array(), array $options = array()) {
    $tablename = $this->generateTemporaryTableName();
    $this->query('CREATE TEMPORARY TABLE {' . $tablename . '} AS ' . $query, $args, $options);
    return $tablename;
  }

  public function driver() {
    return 'pgsql';
  }

  public function databaseType() {
    return 'pgsql';
  }

  /**
   * Overrides DatabaseConnection::createDatabase().
   *
   * @param string $database
   *   The name of the database to create.databaseType
   *
   * @throws DatabaseNotFoundException
   */
  public function createDatabase($database) {
    throw new DatabaseOperationNotSupported('Unsupported operation.');
  }

  public function mapConditionOperator($operator) {
    static $specials;

    // Function calls not allowed in static declarations, thus this method.
    if (!isset($specials)) {
      $specials = array(
        // In PostgreSQL, 'LIKE' is case-sensitive. For case-insensitive LIKE
        // statements, we need to use ILIKE instead.
        'LIKE' => array('operator' => 'ILIKE'),
        'NOT LIKE' => array('operator' => 'NOT ILIKE'),
        'REGEXP' => array('operator' => '~*'),
        'NOT REGEXP' => array('operator' => '!~*'),
        'REGEXP BINARY' => array('operator' => '~'),
        'NOT REGEXP BINARY' => array('operator' => '!~'),
      );
    }

    return isset($specials[$operator]) ? $specials[$operator] : NULL;
  }

  /**
   * Retrive a the next id in a sequence.
   *
   * PostgreSQL has built in sequences. We'll use these instead of inserting
   * and updating a sequences table.
   */
  public function nextId($existing = 0) {

    // Retrive the name of the sequence. This information cannot be cached
    // because the prefix may change, for example, like it does in simpletests.
    $sequence_name = $this->makeSequenceName('sequences', 'value');

    // When PostgreSQL gets a value too small then it will lock the table,
    // retry the INSERT and if it's still too small then alter the sequence.
    $id = $this->query("SELECT nextval('" . $sequence_name . "')")->fetchField();
    if ($id > $existing) {
      return $id;
    }

    // PostgreSQL advisory locks are simply locks to be used by an
    // application such as Drupal. This will prevent other Drupal proccesses
    // from altering the sequence while we are.
    $this->query("SELECT pg_advisory_lock(" . POSTGRESQL_NEXTID_LOCK . ")");

    // While waiting to obtain the lock, the sequence may have been altered
    // so lets try again to obtain an adequate value.
    $id = $this->query("SELECT nextval('" . $sequence_name . "')")->fetchField();
    if ($id > $existing) {
      $this->query("SELECT pg_advisory_unlock(" . POSTGRESQL_NEXTID_LOCK . ")");
      return $id;
    }

    // Reset the sequence to a higher value than the existing id.
    $this->query("ALTER SEQUENCE " . $sequence_name . " RESTART WITH " . ($existing + 1));

    // Retrive the next id. We know this will be as high as we want it.
    $id = $this->query("SELECT nextval('" . $sequence_name . "')")->fetchField();

    $this->query("SELECT pg_advisory_unlock(" . POSTGRESQL_NEXTID_LOCK . ")");

    return $id;
  }

  public function utf8mb4IsActive() {
    return TRUE;
  }

  public function utf8mb4IsSupported() {
    return TRUE;
  }

  public function setSessionTimezoneOffset($offset) {
    $this->query("SET TIME ZONE INTERVAL :offset HOUR TO MINUTE", array(':offset' => $offset));
  }

  public function hasTimezoneSupport() {
    $test = db_query("SELECT '2008-02-15 12:00:00 UTC' AT TIME ZONE 'US/Central'")->fetchField();
    return ($test == '2008-02-15 06:00:00');
  }

  public function dateFieldSql($field, $field_type = DATE_UNIX) {
    switch ($field_type) {
      case 'int':
        watchdog('database', 'Date field type "int" has been deprecated, use DATE_UNIX instead.', array(), WATCHDOG_DEPRECATED);
      case DATE_UNIX:
        $field = "TO_TIMESTAMP($field)";
        break;

      case DATE_ISO:
        $field = "STR_TO_DATE($field, '%Y-%m-%dT%T')";
        break;

      case DATE_DATETIME:
        break;
    }

    return $field;
  }

  public function dateMathSql($field, $direction, $count, $granularity) {
    $granularity = strtoupper($granularity) . 'S';
    switch ($direction) {
      case 'ADD':
        return "($field + INTERVAL '$count $granularity')";

      case 'SUB':
        return "($field - INTERVAL '$count $granularity')";
    }
    return $field;
  }

  public function dateFormatSql($field, $format) {
    $replace = array(
      'Y' => 'YYYY',
      'y' => 'YY',
      'M' => 'Mon',
      'm' => 'MM',
      'n' => 'MM', // no format for Numeric representation of a month, without leading zeros
      'F' => 'Month',
      'D' => 'Dy',
      'd' => 'DD',
      'l' => 'Day',
      'j' => 'DD', // no format for Day of the month without leading zeros
      'W' => 'WW',
      'H' => 'HH24',
      'h' => 'HH12',
      'i' => 'MI',
      's' => 'SS',
      'A' => 'AM',
    );
    $format = strtr($format, $replace);
    return "TO_CHAR($field, '$format')";
  }

  public function dateExtractSql($field, $extract_type) {
    // Note there is no space after FROM to avoid db_rewrite problems
    // see http://drupal.org/node/79904.
    switch (strtoupper($extract_type)) {
      case 'DATE':
        return $field;
      case 'YEAR':
        return "EXTRACT(YEAR FROM($field))";
      case 'MONTH':
        return "EXTRACT(MONTH FROM($field))";
      case 'DAY':
        return "EXTRACT(DAY FROM($field))";
      case 'HOUR':
        return "EXTRACT(HOUR FROM($field))";
      case 'MINUTE':
        return "EXTRACT(MINUTE FROM($field))";
      case 'SECOND':
        return "EXTRACT(SECOND FROM($field))";

      // ISO week number for date.
      case 'WEEK':
        return "EXTRACT(WEEK FROM($field))";
      case 'DOW':
        return "EXTRACT(DOW FROM($field))";
      case 'DOY':
        return "EXTRACT(DOY FROM($field))";
    }

    // An unknown function.
    return FALSE;
  }

  public function dateConvertTimezoneSql($field, $source, $target) {
    return "$field AT TIMEZONE '$target'";
  }
}

/**
 * @} End of "addtogroup database".
 */
