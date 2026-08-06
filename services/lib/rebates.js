// Rebate program catalog and job matcher.
//
// Programs are seeded into the store on first boot so the admin can edit,
// deactivate, or add programs in the UI and have those changes stick. The
// matcher is deliberately conservative: it matches on job category and the
// program's validity window, and every seeded program carries a "verify
// before quoting" note — rebate rules change under contractors constantly,
// and a wrong promise to a homeowner costs more than a missed match.

export const JOB_CATEGORIES = {
  hvac_heat_pump: 'Heat pump (ducted or mini-split)',
  hvac_furnace_ac: 'Furnace, boiler, or central AC',
  water_heater: 'Water heater (tank, tankless, or heat pump)',
  insulation_air_sealing: 'Insulation & air sealing',
  windows_doors: 'Windows & exterior doors',
  electrical_panel: 'Electrical panel or wiring upgrade',
  solar_battery: 'Solar or battery storage',
  appliances: 'Appliances',
  roofing: 'Roofing',
  plumbing: 'Plumbing',
  other: 'Other',
};

export const REBATE_STATUSES = {
  new: 'New — not yet reviewed',
  potential: 'Potential rebate identified',
  submitted: 'Rebate application submitted',
  paid: 'Rebate paid',
  none: 'No rebate available',
};

// Levels: federal | state | utility | retail. Amounts are indicative text,
// not promises — the admin verifies against the live program before filing.
export const SEED_PROGRAMS = [
  {
    slug: 'federal-25c',
    name: 'Federal 25C — Energy Efficient Home Improvement Credit',
    level: 'federal',
    categories: ['hvac_heat_pump', 'hvac_furnace_ac', 'water_heater', 'insulation_air_sealing', 'windows_doors', 'electrical_panel'],
    amount: 'Up to $3,200/yr (heat pumps up to $2,000)',
    starts_on: null,
    ends_on: '2025-12-31',
    requirements: 'Equipment must meet the applicable ENERGY STAR / CEE efficiency tiers. Homeowner claims the credit on their federal return (IRS Form 5695).',
    notes: 'Terminated for property placed in service after Dec 31, 2025 — still claimable for qualifying 2025 installs.',
    active: true,
  },
  {
    slug: 'federal-25d',
    name: 'Federal 25D — Residential Clean Energy Credit',
    level: 'federal',
    categories: ['solar_battery'],
    amount: '30% of system cost',
    starts_on: null,
    ends_on: '2025-12-31',
    requirements: 'Solar PV, battery storage (3 kWh+), solar water heating. Homeowner claims on IRS Form 5695.',
    notes: 'Terminated for expenditures after Dec 31, 2025 — still claimable for qualifying 2025 installs.',
    active: true,
  },
  {
    slug: 'ira-hear',
    name: 'Home Electrification & Appliance Rebates (HEAR)',
    level: 'state',
    categories: ['hvac_heat_pump', 'water_heater', 'electrical_panel', 'insulation_air_sealing', 'appliances'],
    amount: 'Up to $8,000 heat pump / $1,750 heat pump water heater / $4,000 panel (income-qualified)',
    starts_on: null,
    ends_on: null,
    requirements: 'Income-qualified households. Administered by each state energy office; often applied at point of sale through a registered contractor.',
    notes: 'Availability, funding, and rules vary by state — confirm the state program is open before quoting.',
    active: true,
  },
  {
    slug: 'ira-homes',
    name: 'Home Efficiency Rebates (HOMES)',
    level: 'state',
    categories: ['insulation_air_sealing', 'hvac_heat_pump', 'hvac_furnace_ac', 'windows_doors'],
    amount: 'Scales with modeled or measured whole-home energy savings',
    starts_on: null,
    ends_on: null,
    requirements: 'State-run whole-home retrofit program; usually requires an energy model or measured savings.',
    notes: 'Varies by state; some states have not launched. Confirm before quoting.',
    active: true,
  },
  {
    slug: 'utility-heat-pump',
    name: 'Electric utility heat pump / heat pump water heater rebate',
    level: 'utility',
    categories: ['hvac_heat_pump', 'water_heater'],
    amount: 'Typically $250–$2,000 depending on utility and equipment',
    starts_on: null,
    ends_on: null,
    requirements: "Check the homeowner's electric utility. Usually needs model and AHRI numbers plus proof of installation.",
    notes: 'Record the homeowner utility on the job — it decides which program applies.',
    active: true,
  },
  {
    slug: 'utility-weatherization',
    name: 'Utility insulation, air-sealing & smart-thermostat programs',
    level: 'utility',
    categories: ['insulation_air_sealing', 'hvac_furnace_ac'],
    amount: 'Varies; often per-square-foot insulation incentives and $30–$100 thermostat rebates',
    starts_on: null,
    ends_on: null,
    requirements: "Homeowner's gas or electric utility efficiency program; some require a pre-approval or audit.",
    notes: null,
    active: true,
  },
  {
    slug: 'energy-star-retail',
    name: 'ENERGY STAR / retail instant rebates',
    level: 'retail',
    categories: ['appliances', 'water_heater'],
    amount: 'Varies by retailer and state',
    starts_on: null,
    ends_on: null,
    requirements: 'Point-of-sale discounts on qualifying ENERGY STAR equipment.',
    notes: null,
    active: true,
  },
];

export function seedPrograms(store) {
  const programs = store.collection('programs');
  if (programs.all().length > 0) return;
  for (const program of SEED_PROGRAMS) programs.insert(program);
}

// Returns the active programs a job plausibly qualifies for. Category must
// match; the completion date must fall inside the program's window when the
// program has one. ISO date strings compare correctly as strings.
export function matchJob(job, programs) {
  return programs.filter((p) => {
    if (p.active === false) return false;
    if (!Array.isArray(p.categories) || !p.categories.includes(job.category)) return false;
    const when = job.completed_on || null;
    if (when && p.starts_on && when < p.starts_on) return false;
    if (when && p.ends_on && when > p.ends_on) return false;
    return true;
  });
}
